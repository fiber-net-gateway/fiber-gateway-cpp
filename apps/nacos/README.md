# Nacos Client Library

## Overview

`apps/nacos` provides a reusable Nacos client library for applications under
`apps/`. It is a library module rather than a runnable application.

The current implementation is only a build-system scaffold. It exposes a small
`hello()` function and a unit test so that target discovery, public headers,
linking, allocator setup, LTO, and CTest registration can be verified before
the Nacos protocol implementation is added.

## Targets

- `fiber_nacos`: the concrete static library target.
- `fiber::nacos`: the stable alias that consuming applications should link.
- `fiber_nacos_tests`: the unit-test executable, built when
  `FIBER_BUILD_TESTS=ON` and GoogleTest is available.

The library links `fiber_lib` publicly. Consumers therefore receive the core
Fiber include paths and link dependencies through `fiber::nacos`.

## Layout

```text
apps/nacos/
├── CMakeLists.txt
├── README.md
├── include/
│   └── fiber/
│       └── nacos/
│           └── NacosClient.h
├── src/
│   └── NacosClient.cpp
└── tests/
    └── NacosClientTest.cpp
```

Public headers belong under `include/fiber/nacos/`. Internal headers and
implementations should remain under `src/`. Future protocol files may be added
under `proto/`, with generated protobuf targets kept private to `fiber_nacos`.

## Build

Configure and build the library:

```bash
cmake -S . -B build
cmake --build build --target fiber_nacos
```

Build and run its tests:

```bash
cmake --build build --target fiber_nacos_tests
ctest --test-dir build -R '^NacosClientTest\.'
```

The top-level `apps/CMakeLists.txt` discovers this directory automatically when
`FIBER_BUILD_APPS=ON`.

## Using the Library

An application under `apps/` should link the alias target:

```cmake
target_link_libraries(my_app_core
    PRIVATE
        fiber::nacos)
```

Public headers are then included as:

```cpp
#include <fiber/nacos/NacosClient.h>
```

Use `PUBLIC fiber::nacos` instead of `PRIVATE` only when the consuming target's
public headers expose types declared by this library.

## Current Smoke API

The temporary scaffold exports:

```cpp
fiber::nacos::hello();
```

It writes `hello` followed by a newline to standard output. This API exists only
to verify the module wiring and can be replaced when the real client API is
introduced.

## Planned Build Boundaries

The eventual implementation should preserve these boundaries:

- Public Nacos API and business types in `include/fiber/nacos/`.
- HTTP authentication, gRPC connection management, JSON codecs, and generated
  protobuf messages as private implementation details.
- Existing `fiber_lib` HTTP, DNS, JSON, gRPC, and protobuf-lite facilities
  reused instead of introducing an external gRPC C++ runtime.
- Generated protobuf targets linked privately so consuming applications do not
  depend directly on generated wire types.
