# HTTP/3 single-host benchmark

This directory contains the loopback-only HTTP/3 interoperability, performance,
and lifecycle runners used by the corresponding plan and report under
`feature/`.

The 2026-07-20 post-fix smoke/diagnostic run is recorded in
`feature/lite_nginx_nginx_single_host_http3_rebenchmark_report.md`. It did not
pass the formal capacity gate, so its short performance samples are not a new
baseline.

## Prerequisites

- `build-bench-h3-off/apps/lite_nginx` and
  `build-bench-h3-on/apps/lite_nginx`;
- `build-bench-h3-off/example/http_benchmark_backend`;
- the repository-pinned Nginx at `temp/nginx-install/sbin/nginx`;
- HTTP/3 h2load and bsslclient under `temp/http3-bench-tools/`;
- user systemd transient units and cgroup v2 accounting.

`prepare_runtime.sh` creates only benchmark certificates, payloads, and runtime
directories under `temp/`. The runners bind all services to loopback ports
18443 and 19001.

## Entry points

```bash
# Closed-loop paired matrix. Defaults are the plan's discovery values; override
# concurrency after checking UDP drop counters on the current host.
SUT_CPUS=0 SUT_QUOTA=none \
  scripts/benchmark/http3/run_matrix.sh

# Exact response hashes, a second HTTP/3 application client, loss/rebinding,
# key-update driver checks, and bounded invalid loopback datagrams.
scripts/benchmark/http3/run_protocol_checks.sh

# Repeated service startup, load, concentrated client disconnect, and recovery.
ROUNDS=20 scripts/benchmark/http3/run_churn.sh

# Trace-only remote-steal proof plus bounded delayed/blocked cancellation,
# upstream abort, and backend restart recovery.
LITE_TRACE_BIN=temp/build-bench-<sha>-trace/apps/lite_nginx \
  scripts/benchmark/http3/run_steal_regression.sh

# Default epoll_pwait2 and forced timerfd fallback, one fresh connection per request.
CONNECTIONS=100 scripts/benchmark/http3/run_short_connections.sh

# Recompute CSV summaries for an existing raw result directory.
scripts/benchmark/http3/summarize.py \
  temp/http3-benchmark-results/<run-id>
```

Useful matrix overrides are `IMPLEMENTATIONS`, `CASE_FILTER`, `REPETITIONS`,
`DURATION`, `WARMUP`, `COOLDOWN`, `H3_CLIENTS`, `H3_STREAMS`, and
`LOAD_THREADS`.

The SUT and backend use systemd `CPUAffinity`, not cgroup `AllowedCPUs`:
the latter is ineffective when the user slice has no delegated cpuset
controller. The client uses `taskset`. Keep `SUT_QUOTA=none` with a single SUT
CPU to avoid CFS quota throttling artifacts.

## Result validity

The primary request rate is the count of logged 2xx requests divided by the
measurement duration. Status 0 at a timed closed-loop cutoff is recorded
separately. A run is not valid for comparison if it has a gate/load failure,
HTTP error, timeout, cgroup throttling, or a positive delta in
`UdpInErrors`, `UdpRcvbufErrors`, or `UdpSndbufErrors`.

All raw logs and external build products stay below `temp/` and are not
intended for Git.
