# Fiber

English | [简体中文](README.zh-CN.md)

`Fiber` is a C++23 framework repository for high-performance gateway and server-side systems. It is not a project with a single main executable. Instead, it is built around a reusable core library `fiber_lib`, a set of single-file examples, a collection of multi-file applications, and a full test suite.

The focus of this project is not just network I/O wrappers. It is a gateway-oriented framework with three primary capability areas: HTTP server-side ingress, including QUIC/HTTP/3 ingress, HTTP client-side upstream access, and scripting support for more expressive and flexible gateway configuration. Under those gateway-facing capabilities, the framework is supported by lower-level building blocks such as event loops, coroutines, synchronization primitives, and high-performance buffer types.

## Project Positioning

- A foundational framework for network infrastructure, gateways, proxies, and server-side components.
- Built around the static library `fiber_lib`, which is shared by examples, tests, and applications.
- The repository currently serves three purposes at once: framework implementation, example-driven validation, and application incubation.
- It does not produce a single default `fiber` executable. Typical build outputs are the library, examples, tests, and applications under `apps/`.

## `fiber_lib` Core Overview

`fiber_lib` is the core of the repository. It is the static library assembled from the modules under `src/`, and it is the shared dependency used by `tests/`, `example/`, and `apps/`. Its purpose is to serve as the foundation of a gateway framework, not just to expose a small set of utility classes or a narrow HTTP wrapper.

From a gateway framework perspective, `fiber_lib` is centered around three core capability areas.

### 1. HTTP Server Capabilities

The first responsibility of a gateway is to accept downstream traffic, so server-side HTTP support is one of the core parts of `fiber_lib`.

- Supports HTTP/1.1 server request handling.
- Supports HTTP/2 server request handling.
- Supports HTTP/3 server request handling over QUIC on UDP listeners.
- Supports TLS termination based on BoringSSL, including QUIC TLS integration for HTTP/3.
- Supports ALPN negotiation between HTTP/1.1 and HTTP/2 on TLS listeners, plus `h3` ALPN for HTTP/3.
- Provides `HttpServer`, `Http1Server`, `Http3Server`, request-exchange abstractions, and transport abstractions for assembling listeners, handlers, and protocol-processing logic.

This part of the framework corresponds to the gateway ingress layer: listening, accepting connections, negotiating protocols, reading requests, writing responses, and acting as the starting point of the forwarding pipeline.

### 2. HTTP Client Capabilities

A gateway must not only receive requests, but also access upstream services efficiently and reliably. Because of that, upstream-facing client capability is another major focus of the framework.

- Supports HTTP/2 client connections and request exchange.
- Supports HTTP/1 connection pooling for upstream connection reuse.
- Supports DNS protocol handling, resolvers, local resolution, and DNS caching.
- Supports composing resolution, connection establishment, pooling, and request exchange into a full upstream access pipeline.

This part of the framework is designed for reverse proxying, forwarding, aggregation, and other gateway-to-upstream access patterns. Compared with a simple one-shot client model, the design here puts more emphasis on reusable connections, cached resolution, and long-term protocol extensibility.

### 3. Scripting Engine Capabilities

Gateway systems often need more than static configuration. They need conditional logic, expression evaluation, and a programmable extension layer. `fiber_lib` provides parsing, IR, interpreter execution, and runtime value support under `src/script/`, with common JSON codec support under `src/common/json/`, to serve as the foundation for more expressive configuration and policy logic.

This capability matters because it enables:

- richer gateway configuration beyond purely static declarative files,
- extensible routing, forwarding, rewriting, conditional checks, and dynamic decisions,
- a shared scripting foundation for future applications, instead of every app inventing its own lightweight DSL.

### Foundational Building Blocks

Under the three gateway-facing capability areas above, `fiber_lib` also provides the low-level infrastructure that makes the framework work:

- `event`
  Event loops, timers, cross-thread notifications, and loop groups.
- `coroutine` / `async`
  C++23 coroutine-based task abstractions, scheduling, and async primitives.
- `mutex`
  Synchronization primitives including `Mutex`, `RWMutex`, `Signal`, and `WaitGroup`.
- `iobuf`
  Buffer and buffer-chain abstractions used in hot-path I/O.
- `net`
  TCP, UDP, Unix domain sockets, TLS streams, and address abstractions.
- `quic`
  QUIC v1 transport primitives, packet and frame codecs, TLS handshake integration, loss recovery, congestion control, stream management, address validation tokens, and UDP endpoint scheduling.
- Other shared infrastructure
  Assertions, error modeling, route matching, memory helpers, and JSON/JS value encoding support.

The value of `fiber_lib` is composition and reuse. Whether the repository grows new reverse proxies, gateway processes, protocol bridges, debugging tools, or experimental applications, they should generally be built on top of these capabilities instead of re-implementing framework infrastructure inside `apps/`.

## Core Modules

- `src/event/`
  Event loops, timers, cross-thread notifications, pollers, and loop groups.
- `src/async/`
  Coroutine tasks, thread groups, mutexes, RW locks, signals, timeouts, and wait groups.
- `src/net/`
  Addresses, listeners, streams, datagram sockets, TLS streams, and low-level fd wrappers.
- `src/quic/`
  QUIC v1 transport implementation, packet processing, crypto integration, congestion and loss handling, stream queues, connection IDs, tokens, and UDP endpoint dispatch.
- `src/dns/`
  DNS messages, cache, resolvers, local resolver support, and address-resolution interfaces.
- `src/http/`
  HTTP/1.1, HTTP/2, HTTP/3, HPACK, QPACK, server-side and client-side exchange types, connection pools, and transport abstractions.
- `src/common/`
  Assertions, I/O errors, route matching, JSON helpers, memory helpers, and shared infrastructure.
- `src/script/`
  Lexing, parsing, IR, interpreter execution, and runtime support.
- `example/`
  Single-file examples focused on minimal runnable usage patterns.
- `apps/`
  Multi-file runnable applications. It currently includes `lite_nginx`, and is intended to host future applications as well.
- `tests/`
  GoogleTest-based test coverage. The repository currently contains 93 `*Test.cpp` files, including QUIC and HTTP/3 coverage.

## Repository Layout

```text
.
├── src/                 # Framework source code, built into fiber_lib
├── example/             # Single-file examples
├── apps/                # Multi-file applications
├── tests/               # GoogleTest / CTest
├── docs/                # Design and module documentation
├── feature/             # Feature notes and design drafts
├── cmake/               # Build scripts and dependency handling
└── scripts/             # Auxiliary scripts
```

## Examples and Applications

### `example/`

`example/` contains minimal runnable programs that demonstrate typical `fiber_lib` usage patterns. The top-level README keeps this brief; detailed example descriptions are available in [example/README.md](example/README.md) and [example/README.zh-CN.md](example/README.zh-CN.md).

### Example Programs

The following examples are built by default:

- `http1_echo`
- `https_echo`
- `tcp_echo`
- `udp_echo`
- `dns_dig`
- `git_http_repo_server`

### `apps/`

`apps/` is the multi-file application directory used for real runnable programs built on top of `fiber_lib`. Compared with `example/`, `apps/` is intended for complete applications rather than minimal demonstrations.

Applications here usually contain:

- their own directory layout and `CMakeLists.txt`,
- application-specific config, runtime assembly, and business modules,
- a production-style command-line entrypoint,
- clearer internal boundaries such as `config/`, `runtime/`, `proxy/`, and `upstream/`,
- their own application-level tests in addition to the shared top-level tests.

`apps/` is not a directory reserved only for the current `lite_nginx` app. It is intended as the application incubation layer on top of the framework. If the repository grows new gateways, proxies, debuggers, protocol bridges, or other experimental services later, they should generally live here as separate sibling applications.

This gives the repository a clear three-layer structure:

- `fiber_lib`
  The common framework capability layer.
- `example/`
  Minimal API usage samples and runnable reference programs.
- `apps/`
  Real applications and application-level architecture built on top of the framework.

### Current Application

- `apps/lite_nginx`
  A lightweight reverse proxy with nginx-style configuration syntax and a deliberately smaller, explicit, non-fully-compatible V1 feature set. TLS listeners can negotiate HTTP/1.1 or HTTP/2, and listeners configured with `http3`/`quic` also bind UDP for downstream HTTP/3 over QUIC and advertise `Alt-Svc`.

## Requirements

- CMake 4.1 or newer
- A compiler with usable C++23 support
- GCC 13+ or Clang 17+

The project checks compiler capability during configuration.

## Dependencies

### Default Dependency

- BoringSSL: used for TLS, HTTPS, and QUIC/HTTP/3 crypto-related features, and fetched automatically by default when needed.

### Optional Dependencies

- GoogleTest: used for tests; can be downloaded automatically when `FIBER_FETCH_DEPS=ON`.
- jemalloc: optional runtime allocator for final executables.

## Quick Start

### 1. Configure and Build

A typical development build:

```bash
cmake -S . -B build
cmake --build build
```

This will typically produce:

- `build/fiber_tests`
- `build/http1_echo`
- `build/https_echo`
- `build/tcp_echo`
- `build/udp_echo`
- `build/dns_dig`
- `build/git_http_repo_server`
- `build/apps/lite_nginx`

### 2. Run Tests

```bash
ctest --test-dir build
```

### 3. Run Examples

HTTP/1 echo:

```bash
./build/http1_echo 8080
```

HTTPS echo:

```bash
./build/https_echo
```

DNS example:

```bash
./build/dns_dig example.com
```

### 4. Run Applications

Start `lite_nginx`:

```bash
./build/apps/lite_nginx
```

Validate the config:

```bash
./build/apps/lite_nginx --check-config
```

## Common Build Options

- `-DFIBER_BUILD_EXAMPLES=OFF`
  Skip building examples under `example/`.
- `-DFIBER_BUILD_APPS=OFF`
  Skip building applications under `apps/`.
- `-DFIBER_BUILD_TESTS=OFF`
  Skip building test targets.
- `-DFIBER_FETCH_DEPS=OFF`
  Disable automatic third-party dependency downloads and require system-installed dependencies.
- `-DFIBER_USE_JEMALLOC=ON`
  Link final executables against jemalloc.
- `-DFIBER_ENABLE_LTO=ON`
  Enable LTO/IPO when supported by the toolchain.
- `-DFIBER_ALLOW_GCC_LTO=ON`
  Explicitly allow GCC LTO; disabled by default to avoid known instability.

Example:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DFIBER_USE_JEMALLOC=ON
cmake --build build-release
ctest --test-dir build-release
```

## Design Direction

- Performance-first, with attention to allocation behavior on hot paths.
- Prefer explicit state and clear lifetimes over wide nullable state and implicit sharing.
- Build high-throughput network paths from `IoBuf`, connection pools, event loops, and coroutine-based control flow.
- Use examples, tests, and applications together to validate framework behavior.

## Documentation Entry Points

- [HTTP/1 Connection Pool](docs/http1-connection-pool.md)
- [lite-nginx Requirements](apps/lite_nginx/README.md)
- The `feature/` directory contains design notes for multiple areas such as HTTP/2, scripting, event loops, streams, and sockets.

## Current Status

The repository already contains a substantial set of network infrastructure and test coverage, but it is better viewed as an evolving framework codebase than as a fully packaged general-purpose SDK. In practice, the fastest way to understand usage is usually to read the examples and `apps/lite_nginx`.

## License

This project is licensed under the [MIT License](LICENSE).
