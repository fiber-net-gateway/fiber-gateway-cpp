# Fiber Gateway

English | [简体中文](README.zh-CN.md)

Fiber Gateway is a performance-first C++23 framework for gateways, reverse
proxies, and asynchronous network services. The repository is centered on the
reusable `fiber_lib` static library; examples, tests, and the modules under
`apps/` build on the same runtime and protocol stack.

This is a framework repository rather than a single executable. A normal build
produces the core library, runnable examples, tests, the `lite_nginx`
application, and optional application-layer libraries such as
`fiber::nacos`, `fiber::cat`, and `fiber::prometheus`.

## Highlights

- Linux `epoll` event loop with timers, cross-thread notification, and
  multi-loop scheduling.
- C++23 coroutine tasks and asynchronous primitives, including mutexes,
  read/write locks, signals, timeouts, wait groups, and versioned watches.
- TCP, UDP, Unix domain sockets, TLS streams, DNS resolution, and DNS caches.
- HTTP/1.1, HTTP/2, and HTTP/3 server stacks.
- HTTP/1.1 connection pooling plus HTTP/2 and HTTP/3 client connections.
- In-tree QUIC v1 transport with TLS, streams, loss recovery, congestion
  control, pacing, connection IDs, address validation, and UDP GSO support
  where available.
- HPACK, QPACK, streaming request and response bodies, and WebSocket proxying
  through HTTP/1 Upgrade or HTTP/2/3 Extended CONNECT.
- A Nacos client library with its own private gRPC/protobuf transport.
- A purpose-built JS-like bytecode engine with compile-time host bindings,
  request-scoped GC, and native coroutine-aware HTTP APIs.
- Common JSON codecs, structured logging, and allocation-conscious buffer and
  memory utilities.

## Purpose-Built Scripting Engine

Gateway behavior often needs to change faster than the native data plane. A
static configuration language is sufficient for wiring listeners and
upstreams, but becomes awkward for conditional routing, request inspection,
header and payload transformation, canary decisions, and multi-step upstream
calls. Implementing every policy in C++ keeps the hot path fast, but turns each
policy adjustment into a rebuild and deployment. Fiber's scripting engine is
the deliberately narrow layer between those two extremes: configuration can
select and compile a policy, while the request still runs inside the same C++
gateway process.

The runtime is an in-tree, JS-like bytecode interpreter rather than an
ECMAScript implementation. It provides integers and floating-point numbers,
strings, binary values, arrays, objects, templates, control flow, exceptions,
and a gateway-oriented standard library. HTTP bindings expose request data,
response construction, route and connection constants, and directive-bound
upstream `request()` and streaming `proxyPass()` operations:

```javascript
directive backend = http "@api";

if ($header.x_canary == "1") {
    return backend.proxyPass({
        headers: {"X-Route": "canary"}
    });
}

resp.sendJson(200, {status: "ready", path: $req.path});
```

Scripts are compiled when configuration is loaded, so unknown functions,
invalid argument counts, unavailable route constants, and invalid directives
fail before serving traffic. The resulting read-only bytecode can be reused
across requests; execution state and garbage-collected values belong to a
request-local `GcHeap`. Host functions are registered explicitly through a
`Library`, which keeps the script-visible capability surface small and makes
the C++/script ABI explicit.

A dedicated engine is important here because embedding a general-purpose
JavaScript runtime would bring another garbage collector and object model, a
larger dependency and ABI surface, and language features unrelated to gateway
policy. It would also require either a second event-loop/asynchronous model or
a substantial bridge to Fiber's scheduler. That integration would work against
Fiber's coroutine ownership and allocation model. Instead, asynchronous host
functions compile to dedicated opcodes: the VM suspends directly on
`fiber::async::Task` and resumes without requiring `Promise` or `await` syntax.
Execution results distinguish returned values, catchable script exceptions,
and host/runtime aborts so the gateway can map failures deliberately.

This engine is therefore not intended to compete with a browser or Node.js
runtime. Its value is that its language surface, bytecode, memory ownership,
and HTTP capabilities can evolve with this gateway's performance and lifecycle
requirements. The explicit capability model reduces exposed machinery, but it
is not by itself a security sandbox; hosts must still apply request-size,
timeout, trust, and resource policies. See the
[script module guide](docs/script-guide.md) for the complete language, standard
library, HTTP API, and C++ embedding contract.

## Repository Layout

```text
.
├── include/fiber # Public fiber_lib headers
├── src/          # Core fiber_lib implementations
├── example/      # Small runnable examples and benchmark helpers
├── apps/         # Applications and optional app-layer libraries
├── tests/        # Core GoogleTest suite
├── docs/         # Stable module documentation
├── feature/      # Design notes, audits, and implementation reports
├── cmake/        # Toolchain, dependency, and target helpers
└── scripts/      # Build, interoperability, and benchmark tooling
```

The main core modules have public headers under `include/fiber/` and
implementations plus private headers under the corresponding `src/` directory.
Only `include/fiber/` is propagated to consumers; `src/` is available privately
to the core library and its white-box tests:

- `event/` — event loops, pollers, timers, and loop groups.
- `async/` — coroutine tasks, scheduling, and synchronization primitives.
- `net/` — socket, listener, stream, TLS, and address abstractions.
- `quic/` — QUIC transport, crypto, recovery, congestion, and streams.
- `http/` — HTTP/1.1, HTTP/2, HTTP/3, clients, servers, and pools.
- `dns/` — DNS messages, clients, resolvers, and caches.
- `common/` — errors, JSON, memory, containers, and shared utilities.
- `script/` and `http_script/` — scripting runtime and HTTP bindings.
- `log/` — logger hierarchy, formatting, and appenders.
- `apps/nacos/` — Nacos client and its private gRPC/protobuf transport.

## Requirements

- Linux. The current event backend is based on `epoll`.
- CMake 4.1 or newer.
- GCC 13+ or Clang 17+ with the C++23 standard library features required by
  the project, including `std::expected`.
- Network access during the first configure unless the dependency sources are
  already cached under `temp/_deps` or a different `FIBER_DEPS_DIR`.

CMake prefers a suitable Clang toolchain, then GCC, when no compiler or
toolchain is selected explicitly. BoringSSL and protobuf-lite are core
dependencies managed by the build. GoogleTest is used when tests are enabled;
jemalloc is optional.

## Quick Start

Configure and build all default targets:

```bash
cmake -S . -B build
cmake --build build
```

Run the discovered GoogleTest cases through CTest:

```bash
ctest --test-dir build --output-on-failure
```

Useful default outputs include:

```text
build/fiber_tests
build/example/http1_echo
build/example/https_echo
build/example/dns_dig
build/example/http3_benchmark_server
build/example/http3_benchmark_client
build/apps/lite_nginx
```

For example:

```bash
./build/example/http1_echo 8080
./build/example/dns_dig example.com
./build/apps/lite_nginx --check-config
./build/apps/lite_nginx
```

See [the examples guide](example/README.md) for every example and the HTTP/3
benchmark invocation.

## Build Options

| Option | Default | Purpose |
| --- | --- | --- |
| `FIBER_BUILD_EXAMPLES` | `ON` | Build programs under `example/`. |
| `FIBER_BUILD_TESTS` | `ON` | Build core and application tests when GoogleTest is available. |
| `FIBER_BUILD_APPS` | `ON` | Build runnable applications discovered under `apps/`. |
| `FIBER_BUILD_NACOS` | initial value of `FIBER_BUILD_APPS` | Build the reusable `fiber::nacos` component. |
| `FIBER_BUILD_CAT` | initial value of `FIBER_BUILD_APPS` | Build the reusable `fiber::cat` component. |
| `FIBER_BUILD_PROMETHEUS` | initial value of `FIBER_BUILD_APPS` | Build the reusable `fiber::prometheus` component. |
| `FIBER_BUILD_CAT_DEMO` | `OFF` | Build the CAT demo program. |
| `FIBER_BUILD_PROMETHEUS_BENCHMARK` | `OFF` | Build the Prometheus record-path benchmark. |
| `FIBER_FETCH_DEPS` | `ON` | Allow fetching missing optional dependencies such as GoogleTest and jemalloc. |
| `FIBER_USE_JEMALLOC` | `OFF` | Link final executables against jemalloc. |
| `FIBER_ENABLE_HTTP3` | `ON` | Declared HTTP/3 switch; the current source list includes HTTP/3 regardless of this value. |
| `FIBER_ENABLE_LTO` | `ON` | Enable IPO/LTO when supported. |
| `FIBER_ALLOW_GCC_LTO` | `OFF` | Opt in to GCC LTO, which is disabled by default for stability. |
| `FIBER_ENABLE_UDP_GSO` | `ON` | Compile Linux UDP GSO support when system headers provide it. |
| `FIBER_FORCE_TIMERFD_POLLER` | `OFF` | Force the timerfd-based poll timeout path instead of `epoll_pwait2`. |
| `FIBER_ENABLE_BENCHMARK_TRACE` | `OFF` | Enable benchmark-only hot-path trace counters. |
| `FIBER_USE_LIBCXX` | `OFF` | Use libc++ with Clang. |
| `FIBER_STATIC_LIBCXX` | `ON` | Statically link libc++ runtimes when `FIBER_USE_LIBCXX=ON`. |

`FIBER_FETCH_DEPS=OFF` disables fallback downloads for optional GoogleTest and
jemalloc dependencies. BoringSSL, the zlib source, and protobuf are still
populated by `cmake/Deps.cmake`; use `FIBER_DEPS_DIR` to point at a reusable or
pre-populated source cache.

All downloaded source archives are SHA-256 verified. Restricted-network and
downstream builds can replace an archive URL and its expected digest without
patching Fiber by setting the corresponding cache variables:

| Dependency | URL variable | SHA-256 variable |
| --- | --- | --- |
| BoringSSL | `FIBER_BORINGSSL_URL` | `FIBER_BORINGSSL_SHA256` |
| zlib | `FIBER_ZLIB_URL` | `FIBER_ZLIB_SHA256` |
| protobuf | `FIBER_PROTOBUF_URL` | `FIBER_PROTOBUF_SHA256` |
| GoogleTest | `FIBER_GOOGLETEST_URL` | `FIBER_GOOGLETEST_SHA256` |
| jemalloc | `FIBER_JEMALLOC_URL` | `FIBER_JEMALLOC_SHA256` |

For example, a FetchContent consumer can select an approved mirror while
retaining verification:

```cmake
set(FIBER_PROTOBUF_URL "https://mirror.example/protobuf-v21.12.tar.gz"
    CACHE STRING "" FORCE)
set(FIBER_PROTOBUF_SHA256 "<mirror-archive-sha256>"
    CACHE STRING "" FORCE)
FetchContent_MakeAvailable(fiber_gateway_cpp)
```

A typical release build with jemalloc is:

```bash
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DFIBER_USE_JEMALLOC=ON
cmake --build build-release
ctest --test-dir build-release --output-on-failure
```

## Examples

The single-file programs under `example/` cover HTTP/HTTPS servers, TCP and
UDP echo services, DNS lookup, a Git smart-HTTP server, and HTTP/3 benchmark
client/server tooling. They are intended as compact API references and smoke
tests, not as production application layouts.

Read [example/README.md](example/README.md) for details.

## Applications and Reusable Modules

The top-level build exposes three optional reusable components layered on
`fiber_lib`. Their source currently lives under `apps/`, but that path is an
internal repository detail: in-tree and FetchContent consumers select them
with top-level options and link their stable aliases.

```cmake
include(FetchContent)

FetchContent_Declare(
    fiber_gateway_cpp
    GIT_REPOSITORY https://github.com/fiber-net-gateway/fiber-gateway-cpp.git
    GIT_TAG <pinned-revision>)
set(FIBER_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(FIBER_BUILD_NACOS ON CACHE BOOL "" FORCE)
set(FIBER_BUILD_CAT ON CACHE BOOL "" FORCE)
set(FIBER_BUILD_PROMETHEUS ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(fiber_gateway_cpp)

target_link_libraries(my_gateway PRIVATE
    fiber::nacos
    fiber::cat
    fiber::prometheus)
```

Enabling a component does not add its demos or benchmarks. Tests continue to
follow `FIBER_BUILD_TESTS`; use the dedicated opt-in options for the CAT demo
and Prometheus benchmark.

The reusable components and complete applications are:

- [AI Gateway](https://github.com/fiber-net-gateway/ai-gateway) — the
  repository-owned LLM gateway and the authoritative home of `ai-server`.
  The legacy application code has been removed from this repository; see the
  [migration pointer](apps/ai-server/README.md) for provenance.
- [`apps/lite_nginx`](apps/lite_nginx/README.md) — a lightweight reverse proxy
  with nginx-style configuration. It accepts HTTP/1.1, HTTP/2, and HTTP/3,
  supports TLS and WebSocket tunnelling, and proxies to HTTP/1.1 upstreams with
  pooled connections.
- [`apps/nacos`](apps/nacos/README.md) — the `fiber::nacos` client library with
  authentication, Nacos 2.x gRPC transport, ConfigService, NamingService,
  subscriptions, reconnection, and service-state replay.
- [`apps/cat`](apps/cat/README.md) — the `fiber::cat` native CAT 3.0 client
  library.
- [`apps/prometheus`](apps/prometheus/README.md) — the `fiber::prometheus`
  fixed-schema metrics library with EventLoop-owned shards and Prometheus text
  exposition.

See [apps/README.md](apps/README.md) for module layout and CMake conventions.

## Documentation

- [Examples](example/README.md)
- [Applications](apps/README.md)
- [Script module guide](docs/script-guide.md) ([简体中文](docs/script-guide.zh-CN.md))
- [HTTP/1 connection pool](docs/http1-connection-pool.md)
- [TLS client certificate identity](docs/tls-client-identity.md)
- [Script function signature ABI](docs/script-function-signature-abi.md)
- [HTTP/3 client design](feature/http3_client.md)
- [QUIC client design](feature/quic_client.md)
- [Nacos ConfigService design](feature/nacos_config_service.md)
- [Nacos NamingService design](feature/nacos_naming_service.md)
- [Prometheus metrics design](feature/prometheus_cpp_metrics_design.md)

The `feature/` directory also contains evolving design notes, audit records,
and benchmark reports. Treat those documents as engineering history unless a
file explicitly describes a current contract.

## Project Status

Fiber Gateway is under active development. The repository already contains a
broad protocol and runtime test suite, but APIs and application contracts may
continue to evolve. The examples and module-specific READMEs are the best
starting points for current usage.

## License

Licensed under the [MIT License](LICENSE).
