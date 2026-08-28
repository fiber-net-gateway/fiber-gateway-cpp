# Repository Guidelines

## Project Structure & Module Organization
This repository is not organized around a single default executable. Core framework implementations and private headers live under `src/`, while public headers live under `include/fiber/`; together they are built into the reusable static library `fiber_lib`, which is shared by examples, tests, and applications. Public headers must not include files from `src/`. Core white-box tests may receive `src/` as a private include root, but that path must never be propagated to consumers. Key modules include `event/`, `async/`, `net/`, `dns/`, `http/`, `common/`, and `script` below both roots; shared memory headers live in `include/fiber/common/mem/`, common JSON codec headers live in `include/fiber/common/json/`, and the script value/GC JSON model lives in `include/fiber/script/json/`. Single-file runnable examples live in `example/`. Multi-file applications live in `apps/`; the current application is `apps/lite_nginx`. Tests live in `tests/` and are wired through CTest/GoogleTest. Documentation and design notes live in `docs/` and `feature/`, with build support under `cmake/` and auxiliary tooling under `scripts/`.

## Build, Test, and Development Commands
This project uses CMake and targets C++23. Typical local workflow:

```bash
cmake -S . -B build
cmake --build build
```
The build normally produces `fiber_lib`, `fiber_tests`, example executables such as `http1_echo` and `dns_dig`, and applications such as `build/apps/lite_nginx`.

If you use CLion, it will generate `cmake-build-debug/` in the repo; do not commit build outputs. Tests can be enabled (default) and run via:

```bash
ctest --test-dir build
```

## Coding Style & Naming Conventions
Follow existing C++23 style: 4-space indentation, braces on the same line, and namespaces under `fiber::...`. Class and type names use PascalCase (e.g., `Buffer`, `Generator`). Header guards follow `FIBER_<NAME>_H`. Include public core headers through their namespaced path (for example, `#include <fiber/common/mem/Buffer.h>`); include core-private headers from the private `src` root with quoted paths (for example, `#include "script/ast/Node.h"`). Keep application-private includes local and explicit. Prefer small, focused headers and keep implementations in `.cpp` files.

Do not spend time repeatedly formatting code while implementing a task. Once the task is complete, run `./format_code.sh` to apply the repository-wide formatting rules.

Design state and member variables to be minimal and explicit. Prefer establishing required invariants at construction or initialization boundaries and assert there, instead of carrying nullable members and repeatedly checking `!= nullptr` at every use site. Push nullability checks to the edges when possible, and keep steady-state execution paths simple.

Do not use C++ exceptions in this project. Do not write `throw`, and prefer `noexcept` for internal callback-style functions and other code paths that are required to be non-throwing by design.

When retrieving time in request-handling paths, prefer `fiber::event::EventLoop::current().now();` as the time source.

## Performance & Memory Requirements
Code in this project is performance-first. Pay close attention to memory allocation and release efficiency, and reduce dynamic allocation churn in latency-sensitive paths. In hot code paths, do not use allocation-heavy standard library types such as `std::string`, `std::vector`, and `std::function` by default; only use them when there is a clear non-hot-path justification. Prefer reusable buffers, fixed-size structures, intrusive or custom memory-managed types, and compile-time or lightweight callable abstractions.

## Testing Guidelines
Tests use GoogleTest and CTest. Add new files under `tests/`, name them `*Test.cpp`, and register them by adding sources to the `fiber_tests` target in `CMakeLists.txt`. Run `ctest --test-dir build` after building. Keep tests small and focused on one behavior.

## Commit & Pull Request Guidelines
Use Conventional Commits with clear scopes: `type(scope): subject`. Preferred types: `feat`, `fix`, `refactor`, `perf`, `test`, `build`, `docs`, `chore`. Keep scopes aligned with modules (e.g., `core`, `json`, `mem`, `build`). Example: `feat(json): add generator state stack`. If a change is breaking, add a `BREAKING CHANGE:` footer.

For pull requests, use the same format for the title, include a short summary and motivation, link related issues, and list the exact build/test commands you ran (e.g., `cmake --build build`). Add screenshots only when behavior is user-visible.
