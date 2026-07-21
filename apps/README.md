# Apps Layout

`apps/` is for runnable programs that are larger than the single-file demos in `example/`, and for optional
application-layer static libraries such as `fiber::nacos` and `fiber::prometheus` that depend on `fiber_lib`.

Recommended layout:

```text
apps/
  CMakeLists.txt
  README.md
  _template/
    CMakeLists.txt
    README.md
    src/
      main.cpp
  my_app/
    CMakeLists.txt
    README.md
    src/
      main.cpp
      ...
    tests/
      ...
    config/
      ...
    scripts/
      ...
```

Rules:

- One application or optional application-layer library per subdirectory under `apps/`.
- Each app directory should have its own `README.md`.
- Each directory should have its own `CMakeLists.txt`. Runnable programs use `fiber_add_app(...)`; library modules
  define a namespaced CMake target and must not add a `main()` to that library.
- Directories starting with `_` are ignored by the top-level `apps/CMakeLists.txt`. Use that for templates or private notes.
- Prefer `src/main.cpp` as the entry point for each app.
- Keep app-specific tests under that app directory, usually `tests/`.
- Keep app-specific config, helper scripts, and docs inside that app directory instead of the repo root.

Build behavior:

- `cmake -S . -B build` will include `apps/` when `FIBER_BUILD_APPS=ON`.
- App binaries are emitted under `build/apps/`; library artifacts remain normal CMake target outputs.
- Simple one-file demos remain under `example/` and build to the normal top-level binary directory.

App README structure:

- `Overview`: what the app does and why it exists.
- `Build`: exact CMake configure/build commands.
- `Run`: invocation examples, flags, ports, env vars, and required files.
- `Layout`: a short explanation of the important local files/directories.
- `Notes`: operational constraints, data flow, or known limitations.
