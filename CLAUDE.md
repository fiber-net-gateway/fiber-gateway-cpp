# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Fiber Gateway Framework is a high-performance C++23 asynchronous gateway framework built on C++20 coroutines. It provides HTTP/1, HTTP/2, and HTTP/3 (via lsquic) support, DNS resolution, TLS, and an embedded JavaScript-like scripting language.

## Build Commands

```bash
# Standard build (auto-detects Clang 17+ or GCC 13+)
cmake -S . -B build
cmake --build build

# Build with jemalloc
cmake -S . -B build -DFIBER_USE_JEMALLOC=ON
cmake --build build

# Run tests
ctest --test-dir build

# Run a specific test
./build/fiber_tests --gtest_filter=TestName.TestCase

# Build with libc++ (Clang only)
cmake -S . -B build -DFIBER_USE_LIBCXX=ON
```

## Architecture Overview

### Core Async System (`src/async/`, `src/event/`)

The framework is built on **C++20 coroutines** with a custom coroutine frame allocator for performance. Key components:

- **EventLoop**: Thread-local epoll-based event loop in `src/event/EventLoop.h`. Uses `Poller` (epoll wrapper) for I/O events, `TimerQueue` for timers, and `MpscQueue` for cross-thread notifications.
- **Task\<T\>**: The main coroutine wrapper in `src/async/Task.h`. Returns values and supports co_await.
- **DetachedTask**: Fire-and-forget coroutines in `src/async/Spawn.h`. Use `spawn()` to schedule detached tasks on the event loop.
- **CoroutineFramePool**: Size-classed allocator for coroutine frames to reduce heap allocation overhead.

All async operations (sleep, I/O, timers) integrate with the EventLoop through awaitable types.

### Synchronization Primitives (`src/async/`)

- **Mutex**: Fiber-aware mutex (not OS-level)
- **RWMutex**: Reader-writer lock for fiber context
- **WaitGroup**: Wait for multiple fibers to complete
- **Signal**: Simple notification primitive
- **Sleep/Timeout**: Async sleep and timeout operations

### Networking (`src/net/`)

- **TcpListener/TcpStream**: TCP server and client connections
- **UdpSocket**: UDP socket support with packet handling
- **UnixListener/UnixStream**: Unix domain socket support
- **TlsTcpStream**: TLS wrapper using BoringSSL
- All stream types are coroutine-aware and support `co_await` for async I/O

### HTTP Implementation (`src/http/`)

**HTTP/1** (`Http1Server.h`, `Http1ClientConnection.h`):
- Zero-copy parser with IoBuf integration
- Connection pooling with `Http1ConnectionPoolCore.h`
- Support for keep-alive and pipelining

**HTTP/2** (`Http2Connection.h`, `Http2Stream.h`):
- Full HPACK encoder/decoder implementation
- Stream multiplexing with flow control
- Header compression using static and dynamic tables
- Connection pooling and stream management

**HTTP/3**: Optional support via lsquic (see `http3_demo_lsquic.cpp`)

### DNS (`src/dns/`)

- **DnsClient**: Async DNS client over UDP/TCP
- **DnsResolver**: High-level resolver with caching
- **DnsCache**: Shared DNS cache with TTL support
- **DnsResolverLocal**: Local resolver (uses `/etc/resolv.conf`)

### Scripting (`src/script/`)

Embedded JavaScript-like language with:
- **Parser**: Tokenizer and AST generation (`parse/` subdirectory)
- **Compiler**: AST to bytecode compiler (`ir/` subdirectory)
- **Runtime**: VM with async support (`run/` subdirectory)
- **StdLibrary**: Built-in functions for HTTP, DNS, async operations

## Key Patterns

### Coroutine-based Async Operations

When writing async code, always use coroutines with `co_await`:

```cpp
fiber::async::Task<void> handle_client(fiber::net::TcpStream stream) {
    // Read data asynchronously
    auto data = co_await stream.read_some(buffer);
    // Write response asynchronously
    co_await stream.write_all(response);
}
```

### Memory Allocation

The project is performance-first. Avoid heavy standard library types in hot paths:
- Use `IoBuf` (from `src/common/mem/`) for zero-copy I/O buffers
- Prefer stack allocation or custom allocators over `std::vector`/`std::string`
- Use the `CoroutineFramePool` for coroutine allocation (automatic in Task<T>)

### Event Loop Integration

Every thread that uses async operations must have an EventLoop:
- `EventLoop::current()` returns the thread-local event loop
- Use `spawn()` to schedule work on the current loop
- Use `EventLoopGroup` for multi-threaded servers

## Coding Style

- 4-space indentation, braces on same line
- PascalCase for types (e.g., `TcpStream`, `Task`)
- `FIBER_` prefix for macros
- Header guards: `FIBER_<MODULE>_<NAME>_H`
- Local includes with relative paths (e.g., `#include "../event/EventLoop.h"`)
- Use `FIBER_ASSERT()` for invariants (from `src/common/Assert.h`)
- Use `FIBER_PANIC()` for unrecoverable errors

## Compiler Requirements

- **Clang 17+** with libc++ (optional, via `FIBER_USE_LIBCXX=ON`)
- **GCC 13+** with libstdc++
- Minimum C++23 standard required
- LTO/IPO enabled by default (disable with `FIBER_ENABLE_LTO=OFF`)
- GCC LTO disabled by default due to stability issues (enable with `FIBER_ALLOW_GCC_LTO=ON`)

## Dependencies

- **BoringSSL**: TLS/crypto library (auto-fetched via CMake)
- **jemalloc**: Optional memory allocator
- **GoogleTest**: Testing framework (auto-fetched if not found)
- **lsquic**: Optional HTTP/3 library
