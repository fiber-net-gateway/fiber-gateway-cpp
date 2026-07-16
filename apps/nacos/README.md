# Nacos Client Library

## Overview

`apps/nacos` provides a reusable Nacos client library for applications under
`apps/`. It is a library module rather than a runnable application.

The module currently contains the original `hello()` build-system smoke API
and the first Nacos wire DTOs with pool-backed JSON codecs:

- `ConfigQueryRequest`
- `NotifySubscriberResponse`

Further client, transport, and DTO implementation will be added incrementally.

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
│           ├── NacosClient.h
│           └── dto/
│               ├── Base.h
│               ├── ConfigQueryRequest.h
│               ├── JsonCodec.h
│               └── NotifySubscriberResponse.h
├── src/
│   ├── NacosClient.cpp
│   └── dto/
│       ├── ConfigQueryRequestJson.cpp
│       ├── JsonCodecSupport.h
│       └── NotifySubscriberResponseJson.cpp
└── tests/
    ├── DtoJsonTest.cpp
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
ctest --test-dir build -R '^(NacosClientTest|NacosDtoJsonTest)\.'
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

## DTO JSON API

DTO headers live under `fiber/nacos/dto/`. JSON decoding uses
`fiber::json::JsonParser` and stores decoded strings in the supplied
`fiber::mem::BufPool`. DTO string views must not outlive that pool. String views
assigned by callers remain borrowed and must stay valid through encoding.

Reference fields use `fiber::json::Nullable<T>`:

- `Absent` fields are omitted during encoding.
- `Null` fields are encoded as JSON `null`.
- `Present` fields are encoded with their value.

Unknown JSON fields are skipped. Known fields use strict JSON type checking,
and decoding is transactional: the output DTO is changed only after the full
object parses successfully.

## Smoke API

The temporary scaffold exports:

```cpp
fiber::nacos::hello();
```

It writes `hello` followed by a newline to standard output. This API remains
only as a module wiring smoke test.

## Planned Build Boundaries

The eventual implementation should preserve these boundaries:

- Public Nacos API and business types in `include/fiber/nacos/`.
- HTTP authentication, gRPC connection management, JSON codecs, and generated
  protobuf messages as private implementation details.
- Existing `fiber_lib` HTTP, DNS, JSON, gRPC, and protobuf-lite facilities
  reused instead of introducing an external gRPC C++ runtime.
- Generated protobuf targets linked privately so consuming applications do not
  depend directly on generated wire types.
