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
  An HTTPS server example showing TLS ingress and secure connection handling.
- `tcp_echo.cpp`
  A TCP echo example showing basic stream read/write behavior.
- `udp_echo.cpp`
  A UDP echo example showing datagram send/receive flow.
- `dns_dig.cpp`
  A DNS query example showing resolver usage, address resolution, and result handling.
- `git_http_repo_server.cpp`
  A more complete HTTP server example showing more involved protocol handling and service logic.
- `http3_demo_lsquic.cpp`
  An experimental HTTP/3 example based on `lsquic`; it is built only when the required dependencies are available.

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
