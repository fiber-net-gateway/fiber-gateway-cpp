# DNS + HTTPS upstream benchmark assets

The full test matrix, fairness rules, validity gates, and interpretation notes
are in `feature/lite_nginx_dns_https_benchmark_plan.md`.

## Prerequisites

- `build-bench/apps/lite_nginx`
- `build-bench/example/http_benchmark_backend`
- `temp/nginx-install/sbin/nginx` from `scripts/build_nginx.sh`
- `build/http3-demo/cert.pem` and `key.pem`
- CoreDNS with the `bind`, `health`, `prometheus`, and `file` plugins
- h2load under `temp/http-bench-tools/root`
- `curl`, `dig`, and `rg` for smoke checks

Run the stack in a dedicated container or network namespace whose
`/etc/resolv.conf` contains `nameserver 127.0.0.53`. The suite expects CoreDNS
to own `127.0.0.53:53`; do not change the host resolver to make this work.

## Configuration checks

```bash
scripts/benchmark/dns_https/prepare_runtime.sh

build-bench/apps/lite_nginx \
  --check-config \
  --config scripts/benchmark/dns_https/configs/lite_nginx_sut.conf

temp/nginx-install/sbin/nginx -t -p "$PWD/" \
  -c scripts/benchmark/dns_https/configs/nginx_sut.conf

temp/nginx-install/sbin/nginx -t -p "$PWD/" \
  -c scripts/benchmark/dns_https/configs/nginx_backend_a.conf

temp/nginx-install/sbin/nginx -t -p "$PWD/" \
  -c scripts/benchmark/dns_https/configs/nginx_backend_b.conf
```

Confirm that the exact CoreDNS binary selected for the run contains the needed
plugins:

```bash
coredns -plugins | rg 'bind|health|prometheus|file'
```

CoreDNS parses and validates the Corefile when it starts. Start it inside the
isolated namespace and require both a successful DNS query and an HTTP 200
from `127.0.0.53:18053/health` before starting either SUT.

## Automated diagnostic

`run_diagnostic.sh` creates an unprivileged user/network/mount namespace,
bind-mounts the benchmark resolver file, starts CoreDNS/origin/TLS backend, and
runs a paired P01 sample. Its defaults are intentionally short diagnostics,
not the formal seven-repetition capacity matrix:

```bash
scripts/benchmark/dns_https/run_diagnostic.sh
```

Useful calibration overrides:

```bash
IMPLEMENTATIONS=nginx REPETITIONS=1 DURATION=5 WARMUP=2 CLIENTS=32 \
  scripts/benchmark/dns_https/run_diagnostic.sh
```

The runner stops before load generation when response-body or SNI verification
fails. Raw output is written below `temp/dns-https-benchmark-results/`.

## Smoke-test startup order

The following commands show process order only. A formal run must add the CPU
affinity, cgroup accounting, log capture, readiness loops, cleanup traps, and
alternating implementation order described by the plan.

```bash
scripts/benchmark/dns_https/prepare_runtime.sh

# Terminal/process unit 1
coredns -conf scripts/benchmark/dns_https/configs/Corefile

# Terminal/process unit 2
build-bench/example/http_benchmark_backend 19001

# These two commands daemonize separate backend instances.
temp/nginx-install/sbin/nginx -p "$PWD/" \
  -c scripts/benchmark/dns_https/configs/nginx_backend_a.conf
temp/nginx-install/sbin/nginx -p "$PWD/" \
  -c scripts/benchmark/dns_https/configs/nginx_backend_b.conf

# Terminal/process unit 3
build-bench/apps/lite_nginx \
  --config scripts/benchmark/dns_https/configs/lite_nginx_sut.conf
```

Run either lite-nginx or the Nginx SUT, never both:

```bash
temp/nginx-install/sbin/nginx -p "$PWD/" \
  -c scripts/benchmark/dns_https/configs/nginx_sut.conf
```

Basic verification:

```bash
dig @127.0.0.53 backend-long.dns-bench.test A +short
curl --fail --dump-header - \
  -H 'Host: localhost' \
  http://127.0.0.1:18080/bench/1k \
  -o /tmp/dns-https-benchmark-body
sha256sum /tmp/dns-https-benchmark-body
curl --fail http://127.0.0.53:9153/metrics
```

The response must include:

```text
X-Benchmark-Upstream-SNI: backend-long.dns-bench.test
```

## Fault injection

`prepare_runtime.sh` copies the tracked seed zone to:

```text
temp/dns-https-benchmark-runtime/db.dns-bench.test
```

Modify only that runtime copy. Every change must increment the SOA serial;
CoreDNS checks it once per second. Address-switch tests replace
`backend-switch`'s A record with `127.0.0.2`; NXDOMAIN tests temporarily remove
the selected owner. Save the before/after zone files and exact change
timestamps with each result.

Delay, packet loss, SERVFAIL, malformed responses, and forced UDP truncation
need a DNS fault proxy or CoreDNS fault plugin in front of this authoritative
zone. Keep the authoritative zone unchanged during those cases so the injected
condition is the only independent variable.
