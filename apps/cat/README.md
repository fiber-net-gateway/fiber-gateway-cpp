# CAT Client Library

## Overview

`apps/cat` is the reusable CAT client library for applications under `apps/`.
Consumers should link the `fiber::cat` target.

The module is currently a build scaffold. Its only public API is
`fiber::cat::cat_hello()`, which prints `hello cat` and verifies that the public
headers, library target, and consuming executable are wired correctly. CAT
protocol and reporting support have not been implemented yet.

## Targets

- `fiber_cat`: concrete static library.
- `fiber::cat`: stable alias for consuming applications.
- `fiber_app_cat_hello`: scaffold executable whose output file is `cat_hello`.

The library links `fiber_lib` publicly, so consumers receive the core Fiber
include paths and dependencies through `fiber::cat`.

## Build

```bash
cmake -S . -B build -DFIBER_BUILD_APPS=ON
cmake --build build --target fiber_cat fiber_app_cat_hello
```

## Run

```bash
./build/apps/cat_hello
```

Expected output:

```text
hello cat
```

## Use from another app

Link the consuming target in CMake:

```cmake
target_link_libraries(your_app PRIVATE fiber::cat)
```

Then include the public header and call the scaffold function:

```cpp
#include <fiber/cat/Cat.h>

fiber::cat::cat_hello();
```

## Layout

- `include/fiber/cat/`: public CAT client headers.
- `src/Cat.cpp`: CAT client library implementation.
- `src/main.cpp`: temporary scaffold executable entry point.

## Notes

`cat_hello` is only a build and linkage check. It is not part of the eventual
CAT monitoring behavior and can be removed after real client APIs and focused
tests replace it.
