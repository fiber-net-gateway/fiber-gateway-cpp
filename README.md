# Fiber Gateway Framework
a gateway framework written by c++23

## Documentation

- [HTTP/1 Connection Pool](docs/http1-connection-pool.md)

## Layout

- `src/`: reusable framework/library code.
- `example/`: small single-file demos.
- `apps/`: multi-file runnable programs with one subdirectory per app.
- `tests/`: GoogleTest coverage.

## Build

Debug build:

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug
```

Release build:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

Options:

- `-DFIBER_BUILD_EXAMPLES=OFF` skips `example/`.
- `-DFIBER_BUILD_APPS=OFF` skips `apps/`.
- `-DFIBER_BUILD_TESTS=OFF` skips tests.

Examples are emitted into the main build directory, for example `./build-release/http1_echo`.
Apps are emitted into `build/apps/`, for example `./build-release/apps/my_app`.

## Build with jemalloc

Enable jemalloc for all final executables with:

```bash
cmake -S . -B build -DFIBER_USE_JEMALLOC=ON
cmake --build build
```

This links examples and `fiber_tests` against jemalloc so `malloc/free`, aligned allocations, and most `new/delete` traffic are handled by jemalloc at runtime.
