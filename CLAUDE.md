# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Standard build (auto-detects Clang 17+ or GCC 13+)
cmake -S . -B build
cmake --build build

# Build with jemalloc
cmake -S . -B build -DFIBER_USE_JEMALLOC=ON
cmake --build build

# Build with libc++ (Clang only)
cmake -S . -B build -DFIBER_USE_LIBCXX=ON

# Run all tests (fiber_tests + lite_nginx_tests)
ctest --test-dir build

# Run a single test case
./build/fiber_tests --gtest_filter=TestName.TestCase

# Run lite_nginx app tests
./build/apps-build/lite_nginx/lite_nginx_tests --gtest_filter=TestName.TestCase

# Format code (requires clang-format on PATH)
./format_code.sh
```

Build outputs: `fiber_lib` (static library), `fiber_tests`, examples (e.g., `http1_echo`, `dns_dig`), and apps (`build/apps/lite_nginx`). Dependencies (BoringSSL, GoogleTest, optional jemalloc) are auto-fetched into `temp/_deps/`.

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `FIBER_USE_JEMALLOC` | OFF | Link executables against jemalloc |
| `FIBER_USE_LIBCXX` | OFF | Use libc++ (Clang only) |
| `FIBER_ENABLE_HTTP3` | ON | Build in-tree HTTP/3 (QUIC) support |
| `FIBER_ENABLE_LTO` | ON | Enable LTO/IPO for fiber targets |
| `FIBER_ALLOW_GCC_LTO` | OFF | Allow LTO with GCC (disabled by default due to instability) |
| `FIBER_BUILD_EXAMPLES` | ON | Build single-file examples under `example/` |
| `FIBER_BUILD_TESTS` | ON | Build tests |
| `FIBER_BUILD_APPS` | ON | Build multi-file applications under `apps/` |
| `FIBER_FETCH_DEPS` | ON | Allow CMake to download third-party dependencies |

## Architecture Overview

### Core Async System (`src/async/`, `src/event/`)

Built on **C++20 coroutines** with an epoll-based event loop.

- **EventLoop** (`src/event/EventLoop.h`): Thread-local singleton (`EventLoop::current()`). Integrates `Poller` (epoll), `TimerQueue` (min-heap timers), and `MpscQueue` (lock-free cross-thread notifications). Provides `post()`, `post_local()`, `post_at()` template methods for type-safe handle-based callbacks using intrusive entries and static trampoline functions. Use `EventLoop::current().now()` as the time source in request paths.
- **Task\<T\>** (`src/async/Task.h`): Main coroutine return type. Lazy (initial_suspend = suspend_always). Supports `co_await` and return values.
- **DetachedTask / spawn()** (`src/async/Spawn.h`): Fire-and-forget coroutines. `spawn(loop, factory)` schedules a coroutine on the event loop via `NotifyEntry`.
- **EventLoopGroup** (`src/event/EventLoopGroup.h`): Multi-threaded event loop pool backed by `ThreadGroup`. Used by `HttpServer` for worker threads.
- **Synchronization**: `Mutex`, `RWMutex`, `WaitGroup`, `Signal` — all fiber-aware (suspend/resume), not OS-level blocking. `Sleep`/`Timeout` integrate with the timer queue.

### Networking (`src/net/`)

All stream types are coroutine-aware. Low-level fd wrappers live in `src/net/detail/` (`StreamFd`, `DatagramFd`, `RWFd`, `AcceptFd`, `ConnectFd`, `Efd`, `TlsStreamFd`).

- **TcpListener/TcpStream**: TCP server and client
- **UdpSocket**: UDP with `UdpPacket`
- **UnixListener/UnixStream**: Unix domain sockets
- **TlsTcpStream**: TLS via BoringSSL, with `TlsContext` for server/client configuration
- **IpAddress/SocketAddress/UnixAddress**: Address types

### HTTP (`src/http/`)

**HTTP/1**: Zero-copy parser (`Http1Parser`) operating on `IoBuf`. Server (`Http1Server`, `HttpServer`) and client (`Http1ClientConnection`) with connection pooling (`Http1ConnectionPoolCore`, `LocalHttp1ConnectionPoolSet`, `StealableHttp1ConnectionPoolSet`). Connection group hinting via `Http1ConnectionGroupKey`/`Http1ConnectionGroupHintTable`/`Http1ConnectionBucketIndex`.

**HTTP/2**: Full implementation — HPACK encoder/decoder with static/dynamic tables and Huffman coding, stream multiplexing (`Http2Connection`, `Http2Stream`, `Http2StreamTable`), outbound scheduling (`Http2OutboundScheduler`), frame encoding. Both server (`ServerHttp2Request`, `ServerHttp2Push`) and client (`ClientHttp2Request`, `ClientHttp2Push`, `Http2ClientConnection`).

**HTTP/3**: In-tree implementation built on the QUIC layer (`Http3Connection` wraps `QuicConnection`). No external dependency like lsquic.

**Common**: `HttpExchange` is the request/response abstraction. `HttpHeaders` uses arena allocation from `BufPool`. `HttpServer` auto-negotiates HTTP/1 vs HTTP/2 via TLS ALPN (`TlsAlpn`). Error handling uses `IoResult<T> = std::expected<T, IoErr>`.

### QUIC (`src/quic/`)

In-tree QUIC transport implementation: `QuicConnection`, `QuicStream`, `QuicStreamReassembler`, `QuicStreamTable`, `QuicPacketCodec`, `QuicPacketProcessor`, `QuicSendScheduler`, `QuicAckHandler`, `QuicCongestion` (congestion control), `QuicCrypto`/`QuicTlsSession` (TLS 1.3 handshake), `QuicUdpEndpoint`, `QuicFrame`, `QuicTransportCodec`/`QuicTransportParamsCodec`.

### DNS (`src/dns/`)

- **DnsClient**: Async DNS wire-protocol client (UDP/TCP)
- **DnsResolver**: High-level resolver with caching
- **DnsCache**: Shared cache with TTL
- **DnsResolverLocal**: Reads `/etc/resolv.conf`
- **DnsMessage/DnsName**: DNS wire-format encoding/decoding

### Scripting (`src/script/`)

Embedded JavaScript-like language:
- **parse/**: `Tokenizer` → `Parser` → AST (`ast/` node types)
- **ir/**: `Compiler` transforms AST into `Compiled` bytecode
- **run/**: `InterpreterVm` executes bytecode with `Binaries`, `Compares`, `Unaries`, `Access` dispatch
- **json/**: `JsValue` tagged-POD value type with `JsGc` garbage collector, `JsNode` type hierarchy, and `JsValueEncode`
- **std/**: `StdLibrary` — built-in functions for HTTP, DNS, async
- **Script**/`ScriptRun`: Entry point; supports both synchronous and `co_await`-based async execution

### Memory (`src/common/mem/`)

- **IoBuf**: Reference-counted buffer with view semantics (begin/end/pos/last pointers), zero-copy slicing via `retain_slice()`, and `IoBufChain` for scatter-gather I/O. The primary buffer type throughout the codebase.
- **BufPool**: Arena/bump allocator with block and large-allocation paths. Used by `HttpHeaders` and other per-request allocations.
- **Allocator**: Simple malloc/free/realloc wrapper.

### Common Utilities (`src/common/`)

- **IoError.h**: `IoErr` enum + `IoResult<T> = std::expected<T, IoErr>` — the standard result type for all I/O operations
- **Assert.h**: `FIBER_ASSERT()` for invariants, `FIBER_PANIC()` for unrecoverable errors
- **NonCopyable/NonMovable**: CRTP-free base classes to delete copy/move
- **IntrusiveList/IntrusiveRbTree**: Intrusive data structures for zero-allocation container membership

## Key Patterns

### Error Handling
All fallible operations return `IoResult<T>` (`std::expected<T, IoErr>`). **No exceptions.** Do not write `throw`. Use `noexcept` for internal callbacks and design-non-throwing paths. `FIBER_ASSERT()` for invariant violations, `FIBER_PANIC()` for unrecoverable errors.

### Event Loop Integration
Every thread running async operations needs an `EventLoop`. Use `EventLoop::current()` to access it. Use `spawn(loop, ...)` to schedule coroutines. Use `EventLoop::current().now()` for timestamps in request paths instead of calling system clocks directly.

### Async I/O
```cpp
fiber::async::Task<void> handle(fiber::net::TcpStream stream) {
    auto data = co_await stream.read_some(buffer);
    co_await stream.write_all(response);
}
```

### Performance
Performance-first codebase. In hot paths: avoid `std::string`, `std::vector`, `std::function`. Use `IoBuf` for buffer management, `BufPool` for arena allocation, intrusive data structures, and compile-time or lightweight callable abstractions. Minimize dynamic allocation churn.

### Nullability at Edges
Design state and member variables to be minimal and explicit. Establish required invariants at construction/initialization boundaries and assert there. Push nullability checks to the edges — keep steady-state execution paths simple and free of repeated `!= nullptr` checks.

## Coding Style

- C++23, 4-space indentation, braces on same line
- PascalCase for types (`TcpStream`, `Task`), `FIBER_` prefix for macros
- Header guards: `FIBER_<MODULE>_<NAME>_H`
- Local includes with relative paths: `#include "../event/EventLoop.h"`
- Namespaces under `fiber::...` (`fiber::async`, `fiber::event`, `fiber::net`, `fiber::http`, `fiber::quic`, `fiber::dns`, `fiber::script`, `fiber::json`, `fiber::common`, `fiber::mem`)
- Tests: `tests/*Test.cpp`, auto-discovered by CMake into `fiber_tests` target

## Project Structure

- `src/` — Core framework, built into static library `fiber_lib`
- `example/` — Single-file runnable examples (e.g., `http1_echo.cpp`, `dns_dig.cpp`)
- `apps/` — Multi-file applications. Currently `apps/lite_nginx` (lightweight reverse proxy with its own config parser, tests, and CMakeLists)
- `tests/` — GoogleTest files (`*Test.cpp`), registered via CTest
- `docs/` — Design documentation
- `feature/` — Feature design notes for individual subsystems
- `cmake/` — Build support (toolchain selection, dependency management, executable helpers)

## Commit & PR Conventions

Conventional Commits with scopes: `type(scope): subject`. Types: `feat`, `fix`, `refactor`, `perf`, `test`, `build`, `docs`, `chore`. Scopes match modules (`core`, `json`, `mem`, `build`, `http`, `quic`, `dns`, `script`). Example: `feat(json): add generator state stack`. Breaking changes get a `BREAKING CHANGE:` footer.

PR titles follow the same format. Include summary, motivation, linked issues, and the exact build/test commands run.
