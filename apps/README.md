# Apps Layout

`apps/` is for runnable programs that are larger than the single-file demos in `example/`.

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

- One app per subdirectory under `apps/`.
- Each app directory should have its own `README.md`.
- Each app directory should have its own `CMakeLists.txt` and call `fiber_add_app(...)`.
- Directories starting with `_` are ignored by the top-level `apps/CMakeLists.txt`. Use that for templates or private notes.
- Prefer `src/main.cpp` as the entry point for each app.
- Keep app-specific tests under that app directory, usually `tests/`.
- Keep app-specific config, helper scripts, and docs inside that app directory instead of the repo root.

Build behavior:

- `cmake -S . -B build` will include `apps/` when `FIBER_BUILD_APPS=ON`.
- App binaries are emitted under `build/apps/`.
- Simple one-file demos remain under `example/` and build to the normal top-level binary directory.

App README structure:

- `Overview`: what the app does and why it exists.
- `Build`: exact CMake configure/build commands.
- `Run`: invocation examples, flags, ports, env vars, and required files.
- `Layout`: a short explanation of the important local files/directories.
- `Notes`: operational constraints, data flow, or known limitations.
