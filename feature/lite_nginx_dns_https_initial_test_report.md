# lite-nginx DNS + HTTPS upstream initial test report

Date: 2026-07-24  
Environment: WSL, Linux user/network/mount namespace, 10 physical cores / 20 SMT
threads  
Status: **functional blocker found; paired performance comparison not valid**

## Versions

- lite-nginx: `build-bench/apps/lite_nginx`
- Nginx: repository-pinned 1.31.3
- CoreDNS: official v1.14.6 linux/amd64 release, archive SHA-256 verified
- Load generator: repository-prepared h2load

The test namespace owns its loopback network and sees a bind-mounted
`/etc/resolv.conf` containing `nameserver 127.0.0.53`. CoreDNS binds
`127.0.0.53:53`, so lite-nginx exercises its real `resolv.conf` DNS path without
changing the WSL host resolver.

## Results

| Test | Result | Evidence |
|---|---|---|
| CoreDNS authoritative zone | PASS after one fixture fix | Default resolver returned `127.0.0.1`; multi-A returned `127.0.0.2`, then `127.0.0.1` |
| lite-nginx DNS + HTTP control | PASS | HTTP 200; origin/proxy SHA-256 both `2edc9868...f292e4a`; one A and one AAAA query |
| lite-nginx DNS + HTTPS upstream | **FAIL** | HTTP 400 from TLS backend: `The plain HTTP request was sent to HTTPS port`; one A and one AAAA query |
| Nginx DNS + HTTPS control | PASS | 392,451/392,451 successful responses, no request error or timeout |

The CoreDNS seed zone originally used one shell-style `#` comment. CoreDNS
correctly rejected it because a DNS master file uses `;` comments. The tracked
fixture was corrected before any functional result was recorded.

The existing `lite_nginx_tests` binary still passes all 67 tests. Its current
HTTPS coverage configures or tests a TLS server/client directly; it does not
exercise a lite-nginx named HTTPS upstream through
`acquire_and_connect()`. The runtime failure therefore identifies a coverage
gap as well as the implementation defect.

## lite-nginx blocker

Observed sequence:

1. CoreDNS received and answered one A and one AAAA query for
   `backend-long.dns-bench.test`.
2. lite-nginx connected to the resolved `127.0.0.1:19443` peer.
3. The Nginx TLS backend returned HTTP 400 with the exact message
   `The plain HTTP request was sent to HTTPS port`.
4. The origin and the same TLS backend returned HTTP 200 when accessed directly.
5. Changing only the lite-nginx upstream scheme/port to
   `http://backend-long.dns-bench.test:19001` returned HTTP 200 with an exact
   body match.

The code explains the observed wire behavior:

- `apps/lite_nginx/src/upstream/UpstreamConnection.cpp:43-49` sets
  `opts.tls.server_name` for an HTTPS connection key but does not set
  `opts.tls.enabled`.
- `src/net/TlsOptions.h:96-107` defaults `enabled` to `false`.
- `src/http/Http1ClientConnection.cpp:53-78` creates and handshakes a TLS
  transport only when `options_.tls.enabled` is true.

This is a lite-nginx implementation defect, not a DNS failure or a benchmark
configuration mismatch. The current config can parse `https://` and retain the
HTTPS scheme in its connection key, but the dial path still opens a plaintext
HTTP transport.

## Nginx control sample

The Nginx-only control was a single 5-second diagnostic run with 2 seconds of
warm-up, 32 HTTP/1.1 downstream connections, two SUT CPUs, and a pooled HTTPS
upstream:

- successful RPS: 78,490
- p50: 0.30 ms
- p99: 3.65 ms
- p99.9: 6.39 ms
- failed / errored / timeout: 0 / 0 / 0
- requests per measured SUT CPU-second: 49,930

This is only a harness control, not a performance baseline: it has one sample,
runs under WSL, and has no valid lite-nginx pair. The expected
`initgroups(root)` warnings come from the intentionally minimal user namespace
GID map; the namespace root maps to the unprivileged host user.

## Raw results

- lite-nginx HTTPS failure:
  `temp/dns-https-benchmark-results/functional-20260724T232502`
- lite-nginx DNS + HTTP control:
  `temp/dns-https-benchmark-results/dns-http-control-20260724T232535`
- Nginx HTTPS diagnostic control:
  `temp/dns-https-benchmark-results/nginx-control-final-20260724T232625`

## Decision

Do not run or publish P00-P05 comparative performance results from the current
binary. The next valid sequence is:

1. enable TLS in the HTTPS upstream dial options;
2. add a focused HTTPS upstream runtime test that verifies TLS and SNI;
3. rebuild `build-bench/apps/lite_nginx`;
4. rerun F01 and the DNS + HTTP control;
5. only after both pass, run the paired P01-P04 matrix.
