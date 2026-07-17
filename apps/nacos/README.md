# Nacos Client Library

`apps/nacos` is the reusable Nacos client library for applications under
`apps/`. Consumers should link the `fiber::nacos` target.

The current implementation covers authentication and the reusable Nacos gRPC
transport layer:

- Immutable, validated client configuration with multiple server IPs.
- Nacos v3 login and optional fallback to the legacy v1 login endpoint.
- HTTP/1.1 short connections for username/password authentication.
- URL-form encoding of credentials.
- Token state broadcast through `async::Watch<NacosAuthSnapshot>`.
- Timed token refresh, bounded exponential retry, and server failover.
- A single long-lived authentication coroutine; HTTP objects remain local to
  each login attempt.
- A fixed `EventLoop` for every Nacos timer, coroutine, and network operation.
- Graceful shutdown through an internal `Watch<bool>` and `WaitGroup`.
- Wire-compatible protobuf `Payload` framing and Java-compatible internal/config
  JSON DTO codecs.
- Plaintext HTTP/2 ServerCheck and bidirectional ConnectionSetup handshakes.
- SetupAck negotiation and the legacy compatibility-delay handshake path.
- Serialized push responses for ClientDetection, ConnectReset, and unknown
  server requests.
- Heartbeats, bounded retry with jitter, server failover, redirect handling,
  connection generations, and bounded inbound/queued messages.
- Dynamic access-token metadata: token refresh does not rebuild a healthy gRPC
  connection, while unavailable authentication stops it until recovery.

The public ConfigService API, unary get/publish/CAS/remove operations, and the
configuration subscription registry are not implemented yet. The RPC layer is
private infrastructure for those next stages; this library does not currently
expose raw Nacos RPC calls as public API.

## Targets

- `fiber_nacos`: concrete static library.
- `fiber::nacos`: stable alias for consuming applications.
- `fiber_nacos_tests`: unit and local integration tests when tests are enabled.

The library links `fiber_lib` publicly, so consumers receive the core Fiber
include paths and dependencies through `fiber::nacos`.

## Public API

The main headers are:

```cpp
#include <fiber/nacos/NacosAuth.h>
#include <fiber/nacos/NacosClient.h>
#include <fiber/nacos/NacosClientConfig.h>
```

Configuration is created through a validation boundary:

```cpp
fiber::nacos::NacosClientConfigParams params;
params.server_ips = {primary_ip, secondary_ip};
params.username = "nacos";
params.password = "password";
params.http_port = 8848;
params.grpc_port = 9848;

auto config = fiber::nacos::NacosClientConfig::create(std::move(params));
if (!config) {
    // Handle NacosConfigError.
}

auto client = fiber::nacos::NacosClient::create(loop, std::move(*config));
if (!client) {
    // Handle NacosCreateError.
}
```

`server_ips` must contain at least one unicast address. Duplicate addresses are
removed while preserving order. The context path must be absolute and defaults
to `/nacos`.

`NacosClientOptions` controls authentication and gRPC connect/request/handshake
timeouts, heartbeat timing, retry backoff, message/response queue limits, and
refresh timing. `client_ip_override` can replace the local gRPC socket address
used in Payload metadata. By default, a token is refreshed at
`refresh_percent` of its TTL. A refresh failure is broadcast in
`last_error`, but an unexpired token remains usable until its actual expiry.

## Event Loop and Lifecycle

The `EventLoop` passed to `NacosClient::create()` is a permanent client
invariant. Call `start()` and await `shutdown()` on that loop:

```cpp
auto auth = (*client)->subscribe_auth();

auto start_result = (*client)->start();
if (!start_result) {
    // Handle the lifecycle error.
}

auto snapshot = co_await auth.next(0);
if (snapshot.value && snapshot.value->ready()) {
    use_token(snapshot.value->access_token);
}

co_await (*client)->shutdown();
```

The lifecycle is:

```text
Created -> Running -> Stopping -> Stopped
```

During shutdown, the client publishes its internal shutdown signal, stops the
gRPC connection and all generation-local stream/heartbeat/writer tasks, then
awaits both the RPC and authentication coroutines through the `WaitGroup`. An
idle authentication coroutine wakes immediately. If a login HTTP operation is
in progress, shutdown waits only for that current connect or request operation's
configured timeout; the coroutine checks shutdown before starting another HTTP
step, fallback, or server attempt. `shutdown()` is idempotent. The final auth
snapshot has state `Stopped`.

During refresh, each HTTP timeout is also capped by the old token's remaining
lifetime. The same coroutine therefore handles refresh, retry, token expiry, and
shutdown without persistent authentication timer objects.

Destroy a client only before it has started or after `shutdown()` completes.
The destructor asserts this invariant.

## Authentication Behavior

The default API selection is `NacosAuthApiVersion::Auto`:

1. POST `{context_path}/v3/auth/user/login`.
2. If that endpoint returns HTTP 404 or 405, retry
   `{context_path}/v1/auth/users/login`.
3. Remember the working API version for later refresh attempts.

Each attempt starts with the previously successful server, then walks the
configured IP list in order. Authentication requests use
`application/x-www-form-urlencoded` and `Connection: close`.

Before the first authentication result, `AuthSubscriber::current()` returns an
empty Watch snapshot with version `0`. Call `next(0)` to wait for the first
published authentication result. Published `NacosAuthSnapshot` states are:

- `Ready`: a non-expired token is available.
- `Unavailable`: no valid token is available.
- `Stopped`: the client has completed shutdown.

Each Watch snapshot also carries its Watch version. Pass that version to the
next `next(version)` call. `NacosAuthSnapshot::generation` increments after each
successful login or refresh; consumers can use it to cheaply detect token
replacement.

## Build and Test

```bash
cmake -S . -B build
cmake --build build --target fiber_nacos_tests
ctest --test-dir build -R '^(NacosClientTest|NacosClientConfigTest|NacosDtoJsonTest|NacosPayloadTest|NacosGrpcConnectionTest)\.'
```

Authentication and gRPC tests use local scripted HTTP/HTTP2 servers. The optional
`NacosGrpcConnectionTest.RnacosInteropWhenEnabled` test performs a real RPC
handshake when `FIBER_NACOS_TEST_GRPC_PORT` points to an rnacos gRPC listener.

## Layout

Public headers live under `include/fiber/nacos/`. Authentication transport and
lifecycle implementation details remain private in `src/detail/`. Nacos gRPC
infrastructure is private in `src/rpc/`. DTO JSON codecs live under `src/dto/`,
and the wire Payload schema lives under `proto/`.
