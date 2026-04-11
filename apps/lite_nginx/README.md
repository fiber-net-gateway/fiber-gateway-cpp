# lite-nginx

`lite-nginx` is a planned app under `apps/` that provides a lightweight
reverse proxy with nginx-style configuration syntax.

This document captures the agreed requirements and scope before code is
implemented.

## Status

- The app skeleton and config parsing layer are implemented.
- The current executable can bind configured `http.listen` entries and serve a
  fixed `hello lite nginx` response over HTTP or HTTPS.
- Reverse-proxy routing and upstream forwarding are still pending.

## Build

```bash
cmake -S . -B build -DFIBER_BUILD_APPS=ON
cmake --build build --target fiber_app_lite_nginx
```

## Run

Validate the bundled sample config:

```bash
./build/apps/lite_nginx --check-config
```

Run the bundled sample config:

```bash
./build/apps/lite_nginx
```

Validate a custom config file:

```bash
./build/apps/lite_nginx --check-config --config /path/to/lite_nginx.conf
```

## Goals

- Build a runnable reverse-proxy app on top of the existing fiber HTTP stack.
- Use nginx-style directive/block syntax for configuration files.
- Keep the first version small, explicit, and easy to validate.
- Reuse existing `HttpServer`, HTTP/1 client, connection pool, and DNS modules.

## Non-Goals For V1

- Full nginx compatibility.
- Variable support such as `$host`, `$remote_addr`, or `$request_uri`.
- `rewrite`, `if`, regex `location`, `include`, or dynamic config reload.
- WebSocket proxying, cache, gzip, upstream HTTP/2, or active health checks.
- Full logging subsystem compatibility with nginx.

## V1 Functional Scope

- Accept downstream HTTP/1.1 requests on non-TLS listeners.
- Accept downstream HTTPS requests on TLS listeners.
- Negotiate HTTP/1.1 or HTTP/2 automatically on TLS listeners via ALPN.
- Proxy to upstream HTTP/1.1 backends.
- Support multiple `server` blocks.
- Support shared `http`-level listeners reused by all `server` blocks.
- Support `server_name` exact matching.
- Support `location =` exact match and plain prefix match.
- Support upstream groups with round-robin selection.
- Support direct static upstream targets such as `http://127.0.0.1:9001`.
- Support named upstream targets such as `http://backend`.
- Support DNS-based upstream host resolution with local cache.
- Support upstream keepalive connection pooling.
- Support request/response streaming instead of whole-body buffering.
- Map upstream connect/read/write failures into standard gateway errors.

## Supported Configuration Syntax

The config file follows nginx-style directives:

```nginx
directive arg1 arg2 ...;

block_name arg1 arg2 ... {
    child_directive ...;
}
```

Supported lexical rules:

- `#` starts a comment until end of line.
- Quoted strings are allowed.
- Unquoted words are allowed for normal directive arguments.
- Braces and semicolons follow nginx-style layout.

## Supported Directives In V1

Top level:

- `worker_processes <n>;`
- `http { ... }`

`http` block:

- `listen <port>;`
- `listen <port> ssl;`
- `listen <ip:port>;`
- `listen <ip:port> ssl;`
- `upstream <name> { ... }`
- `server { ... }`

`upstream` block:

- `server <host:port>;`
- `keepalive <n>;`
- `connect_timeout <duration>;`
- `read_timeout <duration>;`
- `send_timeout <duration>;`

`server` block:

- `server_name <name> [name ...];`
- `certificate <path>;`
- `certificate_key <path>;`
- `location <pattern> { ... }`
- `location = <pattern> { ... }`
- `proxy_connect_timeout <duration>;`
- `proxy_read_timeout <duration>;`
- `proxy_send_timeout <duration>;`
- `proxy_set_header <name> <literal>;`

`location` block:

- `proxy_pass http://<upstream_name>;`
- `proxy_pass http://<host:port>;`
- `proxy_connect_timeout <duration>;`
- `proxy_read_timeout <duration>;`
- `proxy_send_timeout <duration>;`
- `proxy_set_header <name> <literal>;`
- `proxy_buffering off;`

## Explicit V1 Restrictions

- `proxy_set_header` accepts only literal values.
- `proxy_pass` accepts only static targets.
- No nginx variables are supported in any directive.
- `listen` is only valid in the `http` block.
- `server` blocks must define at least one `server_name`.
- `listen ... ssl` enables TLS; HTTP/2 is negotiated automatically and does not
  need a separate config switch.
- If any `http` listener uses `ssl`, every `server` must define both
  `certificate` and `certificate_key`.
- `server_name` uses exact match only.
- `location` supports exact match and prefix match only.
- Only `http://` upstream targets are supported in V1.
- `proxy_buffering` only accepts `off`.

Examples that are valid in V1:

```nginx
proxy_pass http://backend;
proxy_pass http://127.0.0.1:9001;
proxy_set_header Host backend.internal;
```

Examples that are not valid in V1:

```nginx
proxy_set_header Host $host;
proxy_set_header X-Forwarded-For $remote_addr;
proxy_pass http://backend$request_uri;
location ~ ^/api/ { }
rewrite ^/a/(.*)$ /b/$1 last;
```

## Request Routing Semantics

- Requests are first matched by the selected `http`-level listener.
- Non-TLS listeners accept HTTP/1.1 only.
- TLS listeners terminate HTTPS and negotiate HTTP/1.1 or HTTP/2 automatically.
- Within a listener, `server_name` is matched by exact hostname.
- Within a selected `server`, `location =` has highest priority.
- Plain prefix `location` uses longest-prefix match.
- If no location matches, the request returns `404 Not Found`.

## Proxy Semantics

- The original downstream request method is forwarded unchanged.
- The original downstream request URI is forwarded unchanged.
- Request bodies are streamed to upstream.
- Response bodies are streamed back to downstream.
- Hop-by-hop headers are filtered and never forwarded upstream or downstream.
- If `proxy_set_header` overrides a header, the configured literal value wins.

Headers that must be filtered as hop-by-hop:

- `connection`
- `keep-alive`
- `proxy-connection`
- `transfer-encoding`
- `upgrade`
- `te`
- `trailer`

Default behavior in V1:

- If `Host` is not overridden with `proxy_set_header`, upstream `Host` uses the
  static target host from `proxy_pass`.
- No automatic variable-based forwarding headers are added.

## Error Mapping

- DNS resolution failure: `502 Bad Gateway`
- Upstream connect failure: `502 Bad Gateway`
- Upstream premature close: `502 Bad Gateway`
- Upstream connect timeout: `504 Gateway Timeout`
- Upstream read timeout: `504 Gateway Timeout`
- Upstream send timeout: `504 Gateway Timeout`
- No matching route: `404 Not Found`

## Runtime Design Requirements

- The runtime config should be immutable after load.
- Config parsing and semantic validation must be separate from request handling.
- Listener configuration should be centralized at the `http` level so the app
  can reuse a small number of `HttpServer` instances.
- Each worker event loop should own its local upstream client state.
- DNS resolution should reuse the existing DNS cache and resolver components.
- Upstream connection reuse should use the existing HTTP/1 keepalive pool model.
- Request handling should avoid allocation-heavy whole-body buffering on hot
  paths.

## Planned Directory Layout

```text
apps/lite_nginx/
  CMakeLists.txt
  README.md
  conf/
    lite_nginx.conf
  src/
    main.cpp
    app/
    config/
    runtime/
    proxy/
    upstream/
  tests/
```

## Layout

- `conf/lite_nginx.conf`: bundled sample config used by the current skeleton.
- `src/app/`: process entry and CLI/config loading flow.
- `src/config/`: lexer, parser, directive AST, semantic validation, file loader.
- `src/runtime/`: runtime placeholders for the next phase.
- `src/proxy/`: proxy placeholders for the next phase.
- `src/upstream/`: upstream placeholders for the next phase.
- `tests/`: app-local unit tests for config parsing and validation.

## Planned Module Responsibilities

- `app/`: process startup, signal handling, config loading, server lifecycle.
- `config/`: lexer, parser, directive AST, semantic validation.
- `runtime/`: immutable compiled config, server matcher, location matcher.
- `proxy/`: request forwarding, response relay, header filtering, error mapping.
- `upstream/`: upstream registry, peer selection, DNS resolve, connection pool access.

## Delivery Plan

Phase 1:

- Create app skeleton and requirement docs.
- Implement config lexer/parser/semantic validator.
- Validate supported directives and reject unsupported syntax clearly.

Phase 2:

- Build runtime config objects and route matchers.
- Implement minimal direct proxy for static `proxy_pass http://host:port`.

Phase 3:

- Add named `upstream` groups.
- Add round-robin peer selection.
- Add DNS-based upstream resolution.
- Add keepalive connection pooling.

Phase 4:

- Harden timeout handling and gateway error mapping.
- Add more tests around routing, streaming, and pool reuse.
- Wire `http`-level listeners and per-server TLS identities into runtime
  startup.

## Validation Plan

Unit tests should cover:

- Config lexing and parsing.
- Semantic validation failures.
- `server_name` matching.
- `location` exact and prefix matching.
- Header override and hop-by-hop header filtering.
- `proxy_pass` target parsing.

Integration tests should cover:

- Basic GET proxying.
- Request body streaming.
- Response body streaming.
- Upstream keepalive reuse.
- Upstream group round-robin.
- DNS resolution path.
- Timeout-to-`504` mapping.
- Upstream failure-to-`502` mapping.
