# Examples

English | [简体中文](README.zh-CN.md)

`example/` contains minimal runnable examples built on top of `fiber_lib`. Their goal is to show how a specific capability is composed and used, not to provide a full application structure.

These examples usually have the following characteristics:

- Single-file implementations for quick reading.
- Focus on one topic at a time.
- Minimal application-layer structure and configuration.
- Useful both as API usage references and lightweight smoke tests.

## Example List

- `http1_echo.cpp`
  A minimal HTTP/1.1 server example showing the event loop, request-body reading, and response writing.
- `https_echo.cpp`
  An HTTPS server example showing TLS ingress, HTTP/1.1, HTTP/2, and HTTP/3 over QUIC.
- `http3_benchmark_client.cpp`
  An HTTP/3 load generator with closed-loop and fixed-RPS modes, concurrent QUIC connections/streams,
  response validation, and latency summaries.
- `http3_benchmark_server.cpp`
  A low-overhead HTTP/3 benchmark server with fixed-size response and request-body echo endpoints.
- `tcp_echo.cpp`
  A TCP echo example showing basic stream read/write behavior.
- `udp_echo.cpp`
  A UDP echo example showing datagram send/receive flow.
- `dns_dig.cpp`
  A DNS query example showing resolver usage, address resolution, and result handling.
- `git_http_repo_server.cpp`
  A more complete HTTP server example showing more involved protocol handling and service logic.

## Build

By default, top-level builds also build the examples under `example/`:

```bash
cmake -S .. -B ../build
cmake --build ../build
```

To disable example builds:

```bash
cmake -S .. -B ../build -DFIBER_BUILD_EXAMPLES=OFF
```

## Run

Example executables are emitted into the top-level build directory, for example:

```bash
../build/http1_echo
../build/https_echo
../build/dns_dig example.com
```

Run the HTTP/3 benchmark server and client with the repository demo certificate:

```bash
../build/example/http3_benchmark_server 18443 2 ../build/http3-demo/cert.pem ../build/http3-demo/key.pem

../build/example/http3_benchmark_client \
  https://localhost:18443/bench/1k \
  --connect-to 127.0.0.1:18443 \
  --connections 4 --streams 16 \
  --warmup 1s --duration 10s \
  --expect-status 200 --expect-bytes 1024 \
  --insecure
```

Use `--mode rate --rps N` for a fixed aggregate request rate, or `--help` for all options. The internal
client is useful for development and regression testing; use the independently implemented client under
`scripts/benchmark/http3/` for formal cross-implementation comparisons.
