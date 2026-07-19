---
name: use-rnacos-server
description: Prepare, start, and clean up the repository's r-nacos test server for apps/nacos development and interoperability testing. Use when work on apps/nacos needs a live Nacos-compatible server, an rnacos reproduction, real gRPC/config-service validation, or FIBER_NACOS_TEST_GRPC_PORT tests. Reuse temp/rnacos when present; otherwise obtain a matching official r-nacos release under temp/.
---

# Use r-nacos Server

Treat r-nacos as an interoperability fixture, not as the definition of Nacos client behavior. Keep all downloaded binaries, runtime data, and logs out of tracked source paths.

## Prepare the Binary

1. Work from the repository root.
2. Run `python3 .agents/skills/use-rnacos-server/scripts/ensure_rnacos.py`.
3. If `temp/rnacos` is already an executable file, use it as-is. Do not query Releases, upgrade it, or replace it merely because another version exists.
4. If it is absent, let the script select the latest stable official release asset matching the current OS, CPU, and libc, verify its published SHA-256 digest, and install it as `temp/rnacos`.
5. When reproducing a known server-version behavior, pass `--version <version>` only if the task or existing documentation requires that version. Do not select a prerelease without an explicit reason.
6. If preparation fails, report the unsupported platform, missing asset, network failure, digest mismatch, or invalid archive. Do not silently use a system Nacos installation or an unrelated binary.

## Start an Isolated Server

Use loopback listeners, unused ports, and a fresh runtime directory. Keep the gRPC port equal to the HTTP port plus 1000 unless a test explicitly needs another layout. Do not use `temp/.env` unchanged for isolated tests because it fixes shared ports and a persistent data directory.

Use this shell pattern after confirming the chosen ports are free:

```bash
rnacos_run_dir=$(mktemp -d "${TMPDIR:-/tmp}/fiber-rnacos.XXXXXX")
rnacos_http_port=18848
rnacos_grpc_port=19848

env \
    RNACOS_SDK_HOST=127.0.0.1 \
    RNACOS_HTTP_PORT="$rnacos_http_port" \
    RNACOS_GRPC_PORT="$rnacos_grpc_port" \
    RNACOS_HTTP_CONSOLE_PORT=0 \
    RNACOS_DATA_DIR="$rnacos_run_dir/data" \
    RNACOS_RAFT_NODE_ADDR="127.0.0.1:$rnacos_grpc_port" \
    RUST_LOG=warn \
    ./temp/rnacos >"$rnacos_run_dir/rnacos.log" 2>&1 &
rnacos_pid=$!
```

Record the PID immediately. Do not kill by process name because another developer may be running a separate fixture.

Poll the r-nacos endpoint `http://127.0.0.1:<http-port>/health` until it returns HTTP 200 with `success`, while also checking that the recorded process remains alive. On timeout or early exit, inspect `rnacos.log`, stop only the recorded PID if necessary, and report the startup error. Do not substitute a Nacos Java console health path without verifying that the cached r-nacos version implements it.

## Run apps/nacos Tests

Build the test target when needed, then expose only the isolated gRPC port to the interoperability tests:

```bash
cmake --build build --target fiber_nacos_tests
FIBER_NACOS_TEST_GRPC_PORT="$rnacos_grpc_port" \
    ctest --test-dir build -R 'RnacosInteropWhenEnabled' --output-on-failure
```

Run broader `apps/nacos` tests when the code change warrants them. Keep known fixture differences distinct from protocol requirements; in particular, consult the current `apps/nacos/README.md` before interpreting a server-specific CAS result.

## Clean Up

Always stop the exact process started for the task, including after a failed build or test:

```bash
kill "$rnacos_pid" 2>/dev/null || true
wait "$rnacos_pid" 2>/dev/null || true
```

Retain the isolated runtime directory long enough to inspect failure logs, then remove only that exact directory if cleanup is desired. Never remove or overwrite `temp/rnacos` during normal cleanup; it is the reusable cache for later tasks.
