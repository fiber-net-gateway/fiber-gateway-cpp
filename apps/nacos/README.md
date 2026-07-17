# Nacos Client Library

`apps/nacos` is the reusable Nacos client library for applications under
`apps/`. Consumers should link the `fiber::nacos` target.

The current implementation covers authentication, the reusable Nacos gRPC
transport layer, and ConfigService:

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
- Coroutine-based get, publish, CAS publish, and remove operations with bounded
  owned results and structured errors.
- Shared configuration subscriptions with explicit Pending, Present, NotFound,
  and Stopped states.
- Batched registration, MD5 deduplication, single-flight query synchronization,
  best-effort unregistration, reconnect recovery, and periodic compensating
  registration.

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
to `/nacos`.

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
    auto snapshot = subscription.current();
    while (snapshot.value && snapshot.value->state != fiber::nacos::ConfigState::Stopped) {
        snapshot = co_await subscription.next(snapshot.version);
        if (snapshot.value->state == fiber::nacos::ConfigState::Present) {
            reload(snapshot.value->data.content);
        } else if (snapshot.value->state == fiber::nacos::ConfigState::NotFound) {
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
subscriptions. A new subscription starts in Pending unless the entry already
has a Present or NotFound value. Every subscription snapshot includes the Watch
version; pass that version back to `next(version)`. Destroy or explicitly close
subscriptions on the client EventLoop, before destroying the client.

Transient connection and query failures retain the last Present or NotFound
value. A new connection generation re-registers all live entries, and a
periodic compensating registration covers lost server-side listener state.
During shutdown every live subscription receives Stopped, config operations
reject new work, and all registration/query tasks finish before shutdown
returns.

`NacosClientOptions` controls authentication and gRPC connect/request/handshake
timeouts, heartbeat timing, retry backoff, message/response queue limits,
configuration content/key limits, maximum listen contexts per batch, the
subscription redo interval, and refresh timing. `client_ip_override` can
replace the local gRPC socket address used in Payload metadata. By default, a
token is refreshed at `refresh_percent` of its TTL. A refresh failure is
broadcast in `last_error`, but an unexpired token remains usable until its
actual expiry.

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
ctest --test-dir build -R '^(NacosClientTest|NacosClientConfigTest|NacosDtoJsonTest|NacosPayloadTest|NacosGrpcConnectionTest|NacosConfigServiceTest)\.'
```

Authentication and gRPC tests use local scripted HTTP/HTTP2 servers. The optional
`NacosGrpcConnectionTest.RnacosInteropWhenEnabled` test performs a real RPC
handshake when `FIBER_NACOS_TEST_GRPC_PORT` points to an rnacos gRPC listener.
`NacosConfigServiceTest.RnacosInteropWhenEnabled` additionally verifies real
publish, get, subscribe/push, CAS update, and remove/NotFound behavior.

The repository's rnacos 0.8.2 fixture accepts a publish with a mismatched
`casMd5` instead of returning a conflict. Scripted protocol tests therefore
verify that ConfigService preserves a real server CAS error code, while the
rnacos interoperability test records the fixture-specific acceptance behavior.

## Layout

Public headers live under `include/fiber/nacos/`. Authentication transport and
lifecycle implementation details remain private in `src/detail/`. ConfigService
state and subscription logic live under `src/config/`, and Nacos gRPC
infrastructure remains private in `src/rpc/`. DTO JSON codecs live under
`src/dto/`, and the wire Payload schema lives under `proto/`.
