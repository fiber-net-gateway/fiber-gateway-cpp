# Fiber Gateway

English | [简体中文](README.zh-CN.md)

Fiber Gateway is a performance-first C++23 framework for gateways, reverse
proxies, and asynchronous network services. The repository is centered on the
reusable `fiber_lib` static library; examples, tests, and the modules under
`apps/` build on the same runtime and protocol stack.

This is a framework repository rather than a single executable. A normal build
produces the core library, runnable examples, tests, the `lite_nginx`
application, and optional application-layer libraries such as
`fiber::nacos` and `fiber::prometheus`.

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
- A scripting runtime, common JSON codecs, structured logging, and
  allocation-conscious buffer and memory utilities.

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
implementations under the corresponding `src/` directory:

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
| `FIBER_BUILD_APPS` | `ON` | Build modules discovered under `apps/`. |
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

`apps/` contains complete applications as well as optional libraries layered
on top of `fiber_lib`:

- [`apps/lite_nginx`](apps/lite_nginx/README.md) — a lightweight reverse proxy
  with nginx-style configuration. It accepts HTTP/1.1, HTTP/2, and HTTP/3,
  supports TLS and WebSocket tunnelling, and proxies to HTTP/1.1 upstreams with
  pooled connections.
- [`apps/nacos`](apps/nacos/README.md) — the `fiber::nacos` client library with
  authentication, Nacos 2.x gRPC transport, ConfigService, NamingService,
  subscriptions, reconnection, and service-state replay.
- [`apps/prometheus`](apps/prometheus/README.md) — the `fiber::prometheus`
  fixed-schema metrics library with EventLoop-owned shards and Prometheus text
  exposition.

See [apps/README.md](apps/README.md) for module layout and CMake conventions.

## Documentation

- [Examples](example/README.md)
- [Applications](apps/README.md)
- [HTTP/1 connection pool](docs/http1-connection-pool.md)
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
