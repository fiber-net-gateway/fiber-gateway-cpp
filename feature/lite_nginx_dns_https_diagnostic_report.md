# lite-nginx DNS + HTTPS upstream diagnostic report

Date: 2026-07-24  
Environment: WSL user/network/mount namespace, 10 physical cores / 20 SMT
threads  
Scope: P01 steady-state diagnostic, not a production capacity result

## Outcome

The HTTPS upstream blocker described in
`feature/lite_nginx_dns_https_initial_test_report.md` is fixed. The shared
upstream connection path now enables TLS when its connection key uses the HTTPS
scheme, while retaining the configured hostname as SNI.

This matches the repository-pinned Nginx 1.31.3 control semantics:

- `ngx_http_proxy_pass()` sets `plcf->ssl = 1` for an `https://` URL
  (`temp/nginx-1.31.3/src/http/modules/ngx_http_proxy_module.c:4400-4406`).
- The request path copies that flag to `u->ssl`
  (`temp/nginx-1.31.3/src/http/modules/ngx_http_proxy_module.c:913-918`).

The lite-nginx fix is in
`apps/lite_nginx/src/upstream/UpstreamConnection.cpp:43-50`. A focused runtime
test now proxies an HTTP request through lite-nginx to a real self-signed TLS
HTTP/1.1 upstream and verifies the returned status and body.

## Verification

- `./format_code.sh`: passed.
- `cmake --build build --target lite_nginx_tests`: passed.
- `cmake --build build --target fiber_app_lite_nginx`: passed.
- `cmake --build build-bench --target fiber_app_lite_nginx`: passed.
- `build/apps-build/lite_nginx/lite_nginx_tests --gtest_brief=1`: 68/68
  passed.
- Full `cmake --build build`: passed. The repository-wide CTest run completed
  1,665 registered tests with one unrelated deterministic failure in
  `LlmRoutingTest.AuthorizesPublicAndGroupModelsWithoutHardcodedUserBypass`;
  four external Nginx/r-nacos interoperability tests were skipped. The failing
  AI routing test was rerun directly and failed at its existing authorization
  assertion; it does not exercise lite-nginx, DNS, TLS, or the changed code.
- Post-fix functional run:
  `temp/dns-https-benchmark-results/functional-after-fix-20260724T233911`.

The functional run verified:

- HTTP 200 and an exact 1 KiB origin/proxy body match.
- `X-Benchmark-Backend: A`.
- `X-Benchmark-Upstream-SNI: backend-long.dns-bench.test`, observed at the TLS
  backend rather than inferred from configuration.
- One A and one AAAA lookup by lite-nginx.
- No request error, timeout, or SUT exit.

## P01 paired diagnostic

Raw result:
`temp/dns-https-benchmark-results/paired-after-fix-20260724T233944`

Parameters:

- 2 SUT workers pinned to two physical CPUs.
- 128 HTTP/1.1 downstream clients, one stream per connection.
- 5-second warm-up plus 15-second measured interval.
- 3 repetitions with implementation order reversed on even repetitions.
- DNS name with 300-second TTL, HTTPS/TLS 1.3 upstream, per-worker/loop
  keepalive pool of 256.

| Implementation | Runs | RPS median | p50 ms | p99 ms | p99.9 ms | req/SUT CPU-s | Errors | Exits |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| lite-nginx | 3 | 64,373 | 1.25 | 9.03 | 9.85 | 35,963 | 0 | 0 |
| Nginx 1.31.3 | 3 | 63,502 | 1.15 | 7.33 | 9.51 | 42,543 | 0 | 0 |

Relative to Nginx, the medians in this run show:

- lite-nginx RPS: +1.37%.
- lite-nginx requests per SUT CPU-second: -15.47%, equivalent to 18.30% more
  measured SUT CPU per request.
- lite-nginx p50 / p99 / p99.9: +8.70% / +23.19% / +3.58%.

Per-run successful RPS:

| Repetition | lite-nginx | Nginx |
|---|---:|---:|
| 1 | 69,115 | 63,570 |
| 2 | 64,373 | 63,502 |
| 3 | 62,653 | 61,675 |

All six load-generator runs exited with status 0. Every response counted by
h2load was successful, no request errored or timed out, and both SUTs remained
active after load. Each SUT start generated exactly one A and one AAAA query.
Pre-load and post-load checks in every repetition returned the expected body,
backend identity, and SNI.

The Nginx workers emitted non-fatal `initgroups(root)` messages because the
isolated user namespace intentionally maps only the invoking unprivileged host
user. Requests were still served successfully and the processes stayed active.

## Interpretation and next work

This three-sample WSL diagnostic establishes functional parity for P01 and
shows similar steady-state throughput. It does not establish a capacity winner:
the sample count is below the planned seven, lite-nginx had higher run-to-run
RPS spread, RSS was not collected, and the CPU-efficiency and p99 medians favor
Nginx despite lite-nginx's slightly higher RPS median.

Before publishing a performance conclusion, run the full plan:

1. P00 direct-IP HTTPS control to separate DNS identity from proxy/TLS cost.
2. P01 with at least seven repetitions and paired confidence intervals.
3. P02-P04 for TTL refresh, no-pool TLS handshakes, and multi-address fallback.
4. F02-F15 fault injection and soak tests.
5. A dedicated or bare-metal environment with backend/load-generator headroom,
   RSS/FD/network-drop collection, and no user-namespace warnings.
