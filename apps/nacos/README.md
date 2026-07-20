# Nacos Client Library

`apps/nacos` is the reusable Nacos client library for applications under
`apps/`. Consumers should link the `fiber::nacos` target.

The current implementation covers authentication, the reusable Nacos gRPC
transport layer, and ConfigService:

- Immutable, validated client configuration with multiple server IPs.
- Nacos 2.x authentication flow through its fixed
  `/nacos/v1/auth/users/login` endpoint; no v1/v3 client-version probing.
- HTTP/1.1 short connections for username/password authentication.
- URL-form encoding of credentials.
- Authentication access broadcast through `async::Watch<NacosAuthAccess>`.
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
- ConfigService-owned gRPC lifecycle and dynamic access-token metadata: token
  refresh does not rebuild a healthy connection, and unconfigured
  authentication omits the `accessToken` metadata header.
- Coroutine-based get, publish, CAS publish, and remove operations with bounded
  owned results and structured errors.
- Shared configuration subscriptions with Present/NotFound values, a null
  snapshot while never-synced, and a `Closed` result on shutdown.
- Batched registration, MD5 deduplication, single-flight query synchronization,
  best-effort unregistration, reconnect recovery, and periodic compensating
  registration.

## Targets

- `fiber_nacos`: concrete static library.
- `fiber::nacos`: stable alias for consuming applications.
- `fiber_nacos_tests`: unit and local integration tests when tests are enabled.

The library links `fiber_lib` publicly, so consumers receive the core Fiber
include paths and dependencies through `fiber::nacos`.

## Independent NacosRpc Transport

`src/rpc/NacosRpc` is a new internal, single-connection transport kept separate
from the existing `NacosGrpcConnection`/ConfigService flow. It directly owns a
`GrpcClient`; its long-lived `run()` task connects, sends `ServerCheckRequest`,
opens the bidirectional stream, sends `ConnectionSetupRequest`, processes
server requests, runs heartbeats, and completes only after full gRPC teardown.
It does not select servers, reconnect, or replace the current service
transport. Callers start `run()`, await `wait_ready()` before unary requests,
signal termination with non-blocking `shutdown()`, and use the `run()` result as
the connection completion barrier.

Unary `request()` calls snapshot the current authentication access and encode a
typed DTO into the Nacos `Payload` JSON envelope. Server pushes are decoded by
Payload type and dispatched through fixed-capacity, allocation-free
`NacosBiRequestHandler::add_request_handler<Request, Response>()`
registrations. Each asynchronous `noexcept` handler returns
`Task<IoResult<Response>>`; its `NacosServerRequestContext` exposes the
per-request `BufPool` used to keep response `string_view` values alive through
encoding. One reader coroutine awaits handlers and serializes all
bidirectional responses. Handler `IoErr` values become generic `ErrorResponse`
messages and codes. ClientDetection and ConnectReset remain transport-owned
control messages. A handler must finish in bounded time and must not wait for a
later request on the same bidirectional stream; shutdown waits for the current
handler to return. The handler registry and callback contexts must remain
immutable and alive until `run()` returns.

## Public API

The main headers are:

```cpp
#include <fiber/nacos/NacosClient.h>
#include <fiber/nacos/NacosClientConfig.h>
#include <fiber/nacos/NacosAuthAccess.h>
#include <fiber/nacos/ConfigService.h>
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
to `/nacos`. Username and password must either both be empty or both be
non-empty. When both are empty, HTTP authentication is skipped.

`NacosClient::config_service()` returns the ConfigService instance owned by the
client. Configuration operations run on the client's EventLoop:

```cpp
auto &configs = (*client)->config_service();

auto published = co_await configs.publish("routes", "DEFAULT_GROUP", route_json,
                                          fiber::nacos::ConfigType::Json);
auto queried = co_await configs.get_config("routes", "DEFAULT_GROUP");
if (queried && *queried) {
    use((*queried)->content, (*queried)->md5);
}

auto subscribed = configs.subscribe("routes", "DEFAULT_GROUP");
if (subscribed) {
    auto subscription = std::move(*subscribed);
    auto &sub = subscription.subscriber();
    auto snapshot = sub.current();
    std::uint64_t version = snapshot.version;
    for (;;) {
        snapshot = co_await sub.next(version);
        const auto &result = *snapshot.value;
        if (result.kind == fiber::nacos::ResultKind::Closed) {
            break; // service shutdown
        }
        version = snapshot.version;
        const auto &value = *result.data;
        if (value.state == fiber::nacos::ConfigState::Present) {
            reload(value.content);
        } else if (value.state == fiber::nacos::ConfigState::NotFound) {
            remove_local_config();
        }
    }
}
```

`get_config()` returns an empty optional for a confirmed NotFound response.
`publish()` accepts all six Java-compatible types and an optional CAS MD5; an
empty CAS value is omitted from the wire request. Successful publish and remove
calls do not mutate the local subscription cache optimistically. The cache
changes only after a server notification or registration response causes a
query.

Each `(dataId, group)` has one internal entry regardless of the number of local
subscriptions. `subscription.subscriber().current()` returns a null snapshot
until the first server value arrives; `next(version)` blocks until then. The
watched value is a `SubscriptionResult<ConfigData>`: `kind == Success` carries
the latest `ConfigData` in `data`; `kind == Closed` marks end-of-subscription
(service shutdown), with no `data`. Each snapshot carries the Watch version;
pass that version back to the next `next(version)`. Destroy or
explicitly close subscriptions on the client EventLoop, before destroying the
client.

Transient connection and query failures retain the last Present or NotFound
value. A new connection generation re-registers all live entries, and a
periodic compensating registration covers lost server-side listener state.
During shutdown every live subscription is closed: `next()` resumes with
`ResultKind::Closed` (or `closed()` returns true) and config operations reject
new work. All registration/query tasks finish before shutdown
returns.

`NacosClientOptions` controls authentication and gRPC connect/request/handshake
timeouts, heartbeat timing, retry backoff, message/response queue limits,
configuration content/key limits, maximum listen contexts per batch, the
subscription redo interval, and response size limits. `client_ip_override` can
replace the local gRPC socket address used in Payload metadata. Refresh timing
is intentionally not configurable: it follows the Java Nacos 2.x client rule
`max(5 seconds, tokenTtl * 90%)`.

## Event Loop and Lifecycle

The `EventLoop` passed to `NacosClient::create()` is a permanent client
invariant. Call `start()` and await `shutdown()` on that loop:

```cpp
auto auth = (*client)->subscribe_auth();

auto start_result = (*client)->start();
if (!start_result) {
    // Handle the lifecycle error.
}

auto access = co_await auth.next(0);
if (access.value && access.value->kind == fiber::nacos::NacosAuthAccessKind::Present) {
    use_token(access.value->access_token);
} else if (access.value && access.value->kind == fiber::nacos::NacosAuthAccessKind::InitialFailed) {
    co_await (*client)->shutdown();
    // Fail application startup.
}

co_await (*client)->shutdown();
```

The lifecycle is:

```text
Created -> Running -> Stopping -> Stopped
```

During shutdown, the client publishes its internal shutdown signal and a final
`Stopped` authentication access value. ConfigService owns and stops its gRPC connection and all
generation-local stream/heartbeat/writer tasks. The client then awaits both the
ConfigService and authentication coroutines through its `WaitGroup`. An idle
authentication coroutine wakes immediately. If a login HTTP operation is in
progress, shutdown waits only for that current connect or request operation's
configured timeout; the coroutine checks shutdown before starting another HTTP
step or server attempt. `shutdown()` is idempotent.

The authentication coroutine keeps the current token and refresh deadline as
local variables. There are no persistent generation, state, expiry, or error
fields in the public authentication API.

Destroy a client only before it has started or after `shutdown()` completes.
The destructor asserts this invariant.

## Authentication Behavior

The client implements only the Nacos 2.x interaction flow. Authentication
always posts to `{context_path}/v1/auth/users/login`; this path name is part of
the Nacos 2.x Java/server protocol and does not mean that this client implements
a selectable v1 mode. It does not probe or call the v3 login API.

Each attempt starts with the previously successful server, then walks the
configured IP list in order. Authentication requests use
`application/x-www-form-urlencoded` and `Connection: close`.

Before the first authentication decision, `AuthSubscriber::current()` returns
an empty Watch snapshot with version `0`. Call `next(0)` to wait for the first
published `NacosAuthAccess`. Its states are:

- `NotConfigured`: username and password are both empty. No HTTP login is
  attempted, gRPC may connect without authentication, and `accessToken` is
  omitted from Payload metadata.
- `InitialFailed`: the first complete pass over all configured servers failed.
  It is published once; the authentication coroutine continues retrying in the
  background.
- `Present`: `access_token` is non-empty and contains the current token.
- `Stopped`: client shutdown has begun and `access_token` is empty.

`Present` is the only state with a non-empty `access_token`. Pass each Watch
version to the next `next(version)` call.

The Java-compatible refresh delay is `max(5 seconds, tokenTtl * 90%)`. A refresh
failure keeps the last published `Present` value unchanged and retries after 5
seconds. The Java client terminates its initial authentication stream on the
first failure; this C++ client publishes `InitialFailed` so startup can fail
without hanging, while retaining bounded background recovery. Skipping login
when credentials are both empty is an explicit C++ extension to the local Java
client behavior.

## Build and Test

```bash
cmake -S . -B build
cmake --build build --target fiber_nacos_tests
ctest --test-dir build -R '^(NacosClientTest|NacosClientConfigTest|NacosDtoJsonTest|NacosPayloadTest|NacosRpcTest|NacosGrpcConnectionTest|NacosConfigServiceTest)\.'
```

Authentication and gRPC tests use local scripted HTTP/HTTP2 servers. The optional
`NacosGrpcConnectionTest.RnacosInteropWhenEnabled` and
`NacosRpcTest.RnacosInteropWhenEnabled` perform real RPC handshakes without
configured authentication when `FIBER_NACOS_TEST_GRPC_PORT` points to an rnacos
gRPC listener.
`NacosConfigServiceTest.RnacosInteropWhenEnabled` additionally verifies real
publish, get, subscribe/push, CAS update, and remove/NotFound behavior.

The repository's rnacos 0.8.2 fixture accepts a publish with a mismatched
`casMd5` instead of returning a conflict. Scripted protocol tests therefore
verify that ConfigService preserves a real server CAS error code, while the
rnacos interoperability test records the fixture-specific acceptance behavior.

## Layout

Public headers live under `include/fiber/nacos/`. HTTP authentication transport
and the root lifecycle remain private in `src/detail/`. ConfigService owns the
gRPC connection and keeps its state and subscription logic under `src/config/`;
the reusable gRPC infrastructure remains private in `src/rpc/`. DTO JSON codecs
live under `src/dto/`, and the wire Payload schema lives under `proto/`.
