# Nacos Client Library

This is the reusable Nacos client component. Consumers enable
`FIBER_BUILD_NACOS` and link the stable `fiber::nacos` target; the component's
repository source path is not part of its CMake API.

The current implementation covers authentication, the private Nacos gRPC
transport, ConfigService, NamingService, and a generic client-side
service-discovery registry:

- Immutable, validated client configuration with multiple logical server hosts
  (IPv4, IPv6, or DNS names).
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
- Wire-compatible protobuf `Payload` framing and Java-compatible
  internal/config/naming JSON DTO codecs.
- Plaintext HTTP/2 ServerCheck and bidirectional ConnectionSetup handshakes.
- SetupAck negotiation and the legacy compatibility-delay handshake path.
- Serialized push responses for ClientDetection, ConnectReset, registered
  module handlers, and unknown server requests.
- Heartbeats, bounded retry with jitter, server failover, redirect handling,
  per-attempt RPC reconstruction, and bounded inbound messages.
- ConfigService-owned gRPC lifecycle and dynamic access-token metadata: token
  refresh does not rebuild a healthy connection, and unconfigured
  authentication omits the `accessToken` metadata header.
- NamingService-owned gRPC lifecycle with independent naming labels and the
  same dynamic access-token metadata behavior.
- Bounded ConfigService and NamingService health watches with explicit
  connection phases, failure categories, monotonic counters, and aggregate
  subscription/registration state.
- Coroutine-based get, publish, CAS publish, and remove operations with bounded
  owned results and structured errors.
- Shared configuration subscriptions with Present/NotFound values, a null
  snapshot while never-synced, and a `Closed` result on shutdown.
- Batched registration, MD5 deduplication, single-flight query synchronization,
  best-effort unregistration, reconnect recovery, and periodic compensating
  registration.
- Naming service query and shared subscriptions with Java-compatible immediate
  server-push semantics and `lastRefTime` deduplication.
- Latest-value instance registration, explicit status observation,
  best-effort deregistration, and reconnect replay.
- Reference-counted `(serviceName, group)` discovery handles over
  NamingService subscriptions.
- Application-defined shared state allocated on the first naming notification,
  updated by later pushes, and retired when the subscription is released.
- DNS resolution preserves logical-host identity while trying every resolved
  physical address; address-family ordering remains owned by the supplied
  resolver.

## Targets

- `fiber_nacos`: concrete static library.
- `fiber::nacos`: stable alias for consuming applications.
- `fiber_nacos_tests`: unit and local integration tests when tests are enabled.

The library links `fiber_lib` publicly, so consumers receive the core Fiber
include paths and dependencies through `fiber::nacos`. Its protobuf-generated
payload and gRPC transport headers remain private implementation details;
`fiber_lib` itself does not link or expose protobuf.

## NacosRpc Transport and Reconnection

`src/rpc/NacosRpc` is the internal one-physical-connection transport. Its
private `src/rpc/grpc` implementation owns the HTTP/2 gRPC framing, status,
stream, and protobuf codec support. `NacosRpc` directly owns a `GrpcClient`;
its long-lived `run()` task connects, sends
`ServerCheckRequest`, opens the bidirectional stream, sends
`ConnectionSetupRequest`, processes server requests, runs heartbeats, and
completes only after full gRPC teardown. It deliberately does not reconnect.
Each high-level service selects an endpoint, constructs its own `NacosRpc`,
starts `run()`, awaits `wait_ready()` before using the connection, and treats
the `run()` result as the teardown barrier. The RPC, bidirectional handler
registry, and redirect target are connection-coroutine locals; the service
keeps only a non-owning pointer while that RPC is ready. ConfigService uses a connection
labeled `module=config`; NamingService uses an independent connection labeled
`module=naming`. After a transport close the owner destroys the stopped RPC,
applies redirect/server-failover/backoff policy, and creates a new instance.

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
#include <fiber/nacos/NacosServiceStatus.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/NamingService.h>
#include <fiber/dns/DnsResolver.h>
```

Discovery consumers additionally include:

```cpp
#include <fiber/nacos/discovery/ServiceDiscovery.h>
```

Configuration is created through a validation boundary:

```cpp
fiber::nacos::NacosClientConfigParams params;
params.server_hosts = {"nacos-primary.internal", "10.0.0.12"};
params.username = "nacos";
params.password = "password";
params.http_port = 8848;
params.grpc_port = 9848;

auto config = fiber::nacos::NacosClientConfig::create(std::move(params));
if (!config) {
    // Handle NacosConfigError.
}

auto client = fiber::nacos::NacosClient::create(loop, address_resolver, std::move(*config));
if (!client) {
    // Handle NacosCreateError.
}

auto configs = fiber::nacos::ConfigService::create(**client);
auto naming = fiber::nacos::NamingService::create(**client);
if (!configs || !naming) {
    // Handle NacosCreateError.
}
```

`server_hosts` must contain at least one host without a scheme, path, or port.
IPv4/IPv6 literals must be unicast; DNS names and IP literals are normalized and
duplicates are removed while preserving order. A trailing DNS root dot is
accepted and removed. Username and password must either both be empty or both
be non-empty. When both are empty, HTTP authentication is skipped.

When any configured server is a DNS name, create the client with a valid
`fiber::dns::AddressResolver` bound to the same EventLoop. The resolver is
borrowed and must outlive the client and every ConfigService/NamingService made
from it. Stop those objects before releasing the resolver. For an IP-only list,
the overload without a resolver remains available.

Each authentication or gRPC connection attempt resolves the logical host
through the resolver and tries all returned addresses in resolver order. DNS
cache and TTL behavior therefore come from the supplied resolver. TCP connects
to the resolved address, while HTTP `Host` and gRPC `:authority` keep the
configured logical host and port. Server-requested ConnectReset targets follow
the same host parsing and resolution rules.

`NacosClient` owns authentication state only. `ConfigService::create()` creates
an independent service using the client's immutable configuration,
authentication subscription, and EventLoop. The client does not retain the
service, and multiple ConfigService instances may be created when separate
connections and subscription sets are required. Configuration operations run
on that EventLoop:

```cpp
auto published = co_await (*configs)->publish("routes", "DEFAULT_GROUP", route_json,
                                             fiber::nacos::ConfigType::Json);
auto queried = co_await (*configs)->get_config("routes", "DEFAULT_GROUP");
if (queried && (*queried)->state == fiber::nacos::ConfigState::Present) {
    use((*queried)->content, (*queried)->md5);
}
```

`get_config()` returns a non-null shared snapshot on every successful query. A
confirmed missing config has `state == ConfigState::NotFound`; Present and
NotFound therefore use the same representation in queries and subscriptions.
`publish()` accepts all six Java-compatible types and an optional CAS MD5; an
empty CAS value is omitted from the wire request. Successful publish and remove
calls do not mutate the local subscription cache optimistically. The cache
changes only after a server notification or registration response causes a
query.

Each `(dataId, group)` has one internal entry regardless of the number of local
subscriptions. Callbacks receive `SubscriptionResult<ConfigData>`:
`kind == Success` carries a non-null shared snapshot in `data`, while
`kind == Closed` marks service shutdown and carries no data. The callback
result reference is temporary, but `result.data` may be copied and retained.
Destroy or explicitly close subscriptions on the owner EventLoop before
destroying their ConfigService.

Transient connection and query failures retain the last Present or NotFound
value. Each newly ready physical connection re-registers all live entries, and
a periodic compensating registration covers lost server-side listener state.
The redo timer is a `when_any` branch in the owning connection coroutine, not
a separate detached lifecycle task.
During shutdown every live callback receives `ResultKind::Closed`, handles
become closed, and config operations reject new work. All registration/query
tasks finish before shutdown returns.

`NamingService::create()` follows the same independent ownership model. Naming
operations use the client's fixed EventLoop:

```cpp
auto queried = co_await (*naming)->get("gateway", "DEFAULT_GROUP");

if (queried) {
    route_to((*queried)->hosts);
}

fiber::nacos::Instance instance{
        .ip = "127.0.0.1",
        .port = 8080,
};
auto registration = (*naming)->registry("gateway", "DEFAULT_GROUP", std::move(instance));
```

`Instance::cluster_name` defaults to `DEFAULT`. Applications that use a
deployment-specific cluster must set it explicitly before calling `registry()`.

`get()` first uses a successful live-subscription value for the same key, then
falls back to `ServiceQueryRequest`; query results are not inserted into the
subscription cache. Multiple local subscribers for one `(serviceName, group)`
share one wire subscription. In line with the ploto Java client,
`SubscribeServiceResponse.serviceInfo` is ignored: only the immediately
following `NotifySubscriberRequest` publishes a value, and equal
`lastRefTime` values are deduplicated.

`registry()` returns a move-only `InstanceRegistration`. `update()` keeps the
latest instance while a request is in flight, `subscribe_status()` observes
Pending/Registered/Failed/Closed, and `close()` performs best-effort
deregistration. Every new physical naming connection restores active
subscriptions and re-registers the latest instance values. Creation, update,
close, and destruction of these handles are owner-EventLoop operations.

Both services expose a latest-value health watch:

```cpp
auto config_status = (*configs)->subscribe_status();
auto naming_status = (*naming)->subscribe_status();

auto snapshot = config_status.current();
if (snapshot.value && snapshot.value->connection.rpc_available) {
    // ConfigService currently owns a ready, usable physical RPC.
}
```

`ConfigServiceStatus` and `NamingServiceStatus` contain only bounded enums,
booleans, and fixed-width counters. `NacosConnectionStatus::phase` is one of
Created, Connecting, Ready, ReconnectBackoff, Stopping, or Stopped.
`failure` is the current transition category: None,
AuthenticationUnavailable, Transport, GrpcStatus, Protocol, Server, or
Shutdown. A successful ready transition resets it to None; an intentional
server redirect also reconnects with None rather than reporting a transport
failure. `rpc_available` becomes false before reconnect reconciliation or
shutdown draining begins, so a previous Ready state never leaks into backoff.

The connection counters are monotonic and saturate instead of wrapping:

- `connection_ready_count` increments whenever a physical RPC reaches Ready;
- `disconnect_count` increments when a previously ready RPC ends other than
  through local shutdown;
- `reconnect_attempt_count` increments for every resolved endpoint or failed
  logical target attempted after the first, including server rotation,
  redirect targets, and post-backoff retries.

Subscription summaries count logical `(id, group)` keys, not callback handles.
`registered_count` and `synchronized_count` are independent: a retained cached
value may remain synchronized while a replacement connection still needs to
register it. `pending_count` includes every active key that still needs current
connection registration, an initial value, or in-flight reconciliation.
Naming registration summaries similarly aggregate live registration handles,
currently registered handles, and handles with pending update/deregister work.

Status publishing and all service mutations happen on the client EventLoop.
The returned `async::Watch` subscriber owns immutable snapshots;
`Subscriber::current()` may be read from another thread and `next()` resumes on
the EventLoop from which it is awaited. Acquire subscribers while the service
object is alive, and preserve the normal service shutdown/destruction
contract.

Health snapshots never include a server address, namespace, tenant, Data ID,
group, service name, token, credential, request body, arbitrary error text, or
per-subscription key. Detailed operation failures remain available through the
existing typed ConfigService/NamingService results.

`ServiceDiscovery<StateOps>` uses an intrusive red-black tree keyed by a cached
hash plus `(serviceName, group)`. Multiple move-only leases for the same key
share one entry and one NamingService subscription. It does not contain a
selection policy: `StateOps::State` is application-defined and is owned in
place by the discovery entry.

The state contract is `StateOps::on_init(key, state)`,
`StateOps::on_update(key, state, snapshot)`, and
`StateOps::on_retire(key, state, reason)`; all three callbacks are `noexcept`.
The first successful naming notification constructs `State`, calls `on_init`,
then applies that notification through `on_update`. An empty host list is still
a successful first notification. Later pushes call `on_update` directly.
`co_await Lease::wait_ready()` suspends until the first value, subscription
closure, lease retirement, or shutdown. After it succeeds, `Lease::state()`
returns the entry-owned `State` by reference. Acquisition and readiness waiting
run on the NamingService owner EventLoop.

Dropping the last lease removes the entry from lookup, stops its naming
subscription, and calls `StateOps::on_retire()` once with a reason. A lease may
be destroyed on a request worker: release is posted back to the owner loop, and
the state is always retired and destroyed there. `shutdown()` is a drain
barrier; the owner EventLoop must keep running until leases held by worker
directories or pinned configuration snapshots have been released.

Options are split by owner. `NacosClientOptions` controls only authentication
connect/request timeouts, response limits, and retry backoff.
`ConfigServiceOptions` and `NamingServiceOptions` own their service-specific
limits and each contain independent `NacosRpcOptions` for gRPC timeouts,
heartbeat, reconnect backoff, message limits, TCP options, and
`client_ip_override`. Authentication refresh timing is intentionally not
configurable: it follows the Java Nacos 2.x client rule
`max(5 seconds, tokenTtl * 90%)`.

## Event Loop and Lifecycle

The `EventLoop` passed to `NacosClient::create()` is shared by the client and
every service created from it. Call every `start()` and await every
`shutdown()` on that loop:

```cpp
auto auth = (*client)->subscribe_auth();

auto start_result = (*client)->start();
if (!start_result) {
    // Handle the lifecycle error.
}
auto config_start = (*configs)->start();
auto naming_start = (*naming)->start();

auto access = co_await auth.next(0);
if (access.value && access.value->kind == fiber::nacos::NacosAuthAccessKind::Present) {
    use_token(access.value->access_token);
} else if (access.value && access.value->kind == fiber::nacos::NacosAuthAccessKind::InitialFailed) {
    co_await (*naming)->shutdown();
    co_await (*configs)->shutdown();
    co_await (*client)->shutdown();
    // Fail application startup.
}

co_await (*naming)->shutdown();
co_await (*configs)->shutdown();
co_await (*client)->shutdown();
```

The client and each service have their own lifecycle:

```text
Created -> Running -> Stopping -> Stopped
```

That internal task lifecycle is represented by the public health phase as
Created, Connecting/Ready/ReconnectBackoff, Stopping, and Stopped.

Each service start creates its own connection/reconnect task. ConfigService and
NamingService each use one lifecycle Watch for stop notification and completion: connection-ready
waits, physical connection completion, and reconnect backoff race that lifecycle
with `when_any`; ConfigService also races its periodic subscription-redo timer.
Each physical `NacosRpc` is coroutine-local and is destroyed
only after its unary work has drained. Client shutdown does not invoke or await
service shutdown; it stops only authentication and publishes a final `Stopped`
access value. A running service treats that authentication value as terminal
and begins stopping, but callers must still await that service's `shutdown()`
as its completion barrier. The recommended order is therefore NamingService,
ConfigService, then NacosClient. Every `shutdown()` is idempotent, including
shutdown before start.

Service factories reject invalid service options and reject creation after the
client has stopped. A created service owns safe copies/subscriptions of the
client dependencies it needs; `NacosClient` never stores service pointers.

The authentication coroutine keeps the current token and refresh deadline as
local variables. There are no persistent generation, state, expiry, or error
fields in the public authentication API.

Destroy each object only before it has started or after its own `shutdown()`
completes.
The destructor asserts this invariant.

## Authentication Behavior

The client implements only the Nacos 2.x interaction flow. Authentication
always posts to `/nacos/v1/auth/users/login`, matching the Java client. This
path name is fixed and does not mean that this client implements a selectable
v1 mode. It does not probe or call the v3 login API.

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
cmake -S . -B build -DFIBER_BUILD_APPS=OFF -DFIBER_BUILD_NACOS=ON
cmake --build build --target fiber_nacos_tests
ctest --test-dir build -R '^(NacosClientTest|NacosClientConfigTest|NacosDtoJsonTest|NacosPayloadTest|NacosRpcTest|NacosConfigServiceTest|NacosNamingServiceTest)\.'
```

Authentication and gRPC tests use local scripted HTTP/HTTP2 servers. The optional
`NacosRpcTest.RnacosInteropWhenEnabled` performs a real RPC handshake without
configured authentication when `FIBER_NACOS_TEST_GRPC_PORT` points to an rnacos
gRPC listener.
`NacosConfigServiceTest.RnacosInteropWhenEnabled` additionally verifies real
publish, get, subscribe/push, CAS update, and remove/NotFound behavior.
`NacosNamingServiceTest.RnacosInteropWhenEnabled` verifies real instance
registration, query, subscribe/push, update, and deregistration behavior.

The repository's rnacos 0.8.2 fixture accepts a publish with a mismatched
`casMd5` instead of returning a conflict. Scripted protocol tests therefore
verify that ConfigService preserves a real server CAS error code, while the
rnacos interoperability test records the fixture-specific acceptance behavior.

## Layout

Public headers live under `include/fiber/nacos/`. HTTP authentication transport
and the root lifecycle remain private in `src/detail/`. ConfigService owns the
gRPC connection and keeps its state and subscription logic under `src/config/`;
NamingService owns a separate gRPC connection and keeps its state under
`src/naming/`. The reusable gRPC infrastructure remains private in `src/rpc/`.
Wire DTO models and their JSON codecs remain private in `src/dto/`, and the
wire Payload schema lives under `proto/`. The concrete ConfigService and
NamingService classes are defined only in their `.cpp` files; their internal
headers expose only dependency-injected factories used by the public factories
and protocol tests.
