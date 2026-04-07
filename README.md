# Fiber Gateway Framework
a gateway framework written by c++23

## Documentation

- [HTTP/1 Connection Pool](docs/http1-connection-pool.md)

## Build with jemalloc

Enable jemalloc for all final executables with:

```bash
cmake -S . -B build -DFIBER_USE_JEMALLOC=ON
cmake --build build
```

This links examples and `fiber_tests` against jemalloc so `malloc/free`, aligned allocations, and most `new/delete` traffic are handled by jemalloc at runtime.
