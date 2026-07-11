# lite-nginx

`lite-nginx` is a lightweight reverse proxy under `apps/` that uses
nginx-style directive/block syntax, with a smaller and intentionally
non-compatible V1 feature set.

The top-level config layout is nginx-like, but `location` matching is backed by
`RoutePathMatcher` rather than nginx's native location engine. That difference
is intentional and affects both supported syntax and matching behavior.

## Status

- Config parsing, semantic validation, runtime compilation, and reverse proxy
  forwarding are implemented.
- The current executable can bind configured listeners, select `server` by
  exact `Host`, match `location`, and proxy to upstream HTTP/1.1 backends.
- Named upstreams support round-robin peer selection and optional keepalive
  pooling.

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
- Reuse existing `HttpServer`, HTTP/1 client, and connection pool modules.

## Non-Goals For V1

- Full nginx compatibility.
- Variable support such as `$host`, `$remote_addr`, or `$request_uri`.
- `rewrite`, `if`, regex `location`, `include`, or dynamic config reload.
- DNS-based upstream resolution.
- WebSocket proxying, cache, gzip, upstream HTTP/2, or active health checks.
- Full logging subsystem compatibility with nginx.

## V1 Functional Scope

- Accept downstream HTTP/1.1 requests on non-TLS listeners.
- Accept downstream HTTPS requests on TLS listeners.
- Negotiate HTTP/1.1 or HTTP/2 automatically on TLS listeners via ALPN.
- Accept downstream HTTP/3 on TLS listeners configured with `http3`.
- Proxy to upstream HTTP/1.1 backends.
- Support multiple `server` blocks.
- Support shared `http`-level listeners reused by all `server` blocks.
- Support `server_name` exact matching.
- Support `location =` exact match.
- Support non-exact `location` patterns through `RoutePathMatcher`, including
  `:name`, `*name`, and plain prefix shorthand.
- Support upstream groups with round-robin selection.
- Support direct static upstream targets such as `http://127.0.0.1:9001`.
- Support named upstream targets such as `http://backend`.
- Support upstream keepalive connection pooling for named upstreams with
  `keepalive <n>;`.
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
- `listen <port> ssl http3;`
- `listen <port> ssl quic;`
- `listen <ip:port>;`
- `listen <ip:port> ssl;`
- `listen <ip:port> ssl http3;`
- `listen <ip:port> ssl quic;`
- `upstream <name> { ... }`
- `connection_pool { ... }` — global keepalive pool shared across all upstreams and script
  targets (keyed by peer). `keepalive_size <n>;` (idle per peer; 0 disables pooling, falling
  back to transient connections) and `keepalive_timeout <duration>;`.
- `server { ... }`

`upstream` block:

- `server <host:port> [weight=<n>];` — IP literal + port; `weight` drives smooth weighted
  round-robin peer selection (default 1). Upstreams carry only peers + weight; pool sizing lives
  in `connection_pool`.
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
- `script_file <path>;` - handle the request with a compiled script instead of proxying
  (mutually exclusive with `proxy_pass`).

## Scripting

A `script_file` location runs an embedded JS-like script per request. The script reaches the
inbound request and the outbound response through host functions and can issue upstream HTTP
calls. See `conf/scripts/` for examples (`echo.js`, `inspect.js`, `vars.js`, `proxy.js`).

Request-side functions (sync unless noted): `req.getHeader([name])`, `req.getQuery([name])`,
`req.getUri`, `req.getPath`, `req.getQueryStr`, `req.getMethod`, `req.getCookie([name])`,
`req.readJson` (async), `req.readBinary` (async), `req.discardBody` (async).

Response-side functions: `resp.setHeader(name, value)`, `resp.addHeader(name, value)`,
`resp.sendJson(status, body)` (async), `resp.send(status[, body])` (async), `resp.addCookie(obj)`.

Upstream HTTP (async, require a `connection_pool` block for keepalive reuse):

- `http.request(options)` - issue an upstream request and return
  `{status:int, headers?:object, body:binary}`. `options`: `upstream` (name, leading `@`
  optional) or `url` (`http(s)://host[:port]`; hostnames are resolved via DNS, IP literals skip
  DNS), `method`, `path`, `query` (string or object), `headers`, `body` (binary/string/object),
  `timeout` (ms), `includeHeaders`.
- `http.proxyPass(options)` - forward the inbound request to an upstream and stream its
  response back to the client; returns the upstream status code. `options`: `upstream`/`url`,
  `method`/`path`/`query` (default to the inbound values), `headers`, `responseHeaders`
  (set on the downstream response; `null` removes), `timeout`. WebSocket/101 upgrade is not
  supported.

A target may be bound once with a directive so calls do not re-specify it:

```
directive svc = http "@backend";          // or http "http://1.2.3.4:8080"
let r = svc.request({path: "/items"});    // target pre-bound
let st = svc.proxyPass({});
```

## Explicit V1 Restrictions

- `proxy_set_header` accepts only literal values.
- `proxy_pass` accepts only static targets.
- No nginx variables are supported in any directive.
- `listen` is only valid in the `http` block.
- `server` blocks must define at least one `server_name`.
- `listen ... ssl` enables TLS; HTTP/2 is negotiated automatically and does not
  need a separate config switch.
- `listen ... ssl http3` additionally binds UDP on the same address and port,
  enables HTTP/3 ALPN, and advertises `Alt-Svc` on downstream HTTP/1.1 and
  HTTP/2 responses.
- `http3` and its `quic` alias require `ssl`.
- If any `http` listener uses `ssl`, every `server` must define both
  `certificate` and `certificate_key`.
- `server_name` uses exact match only.
- Non-exact `location` uses `RoutePathMatcher` syntax and semantics, not nginx
  `location` precedence rules.
- Only `http://` upstream targets are supported in V1 (for `proxy_pass`).
- Upstream peers must be configured with IP literals in the current runtime. Script
  `url` targets accept hostnames (resolved via DNS) or IP literals.
- `proxy_buffering` only accepts `off`.
- `http.proxyPass` does not support WebSocket / `101 Switching Protocols` upgrade tunnelling.
- A single global keepalive pool is shared across all upstreams and script targets; per-upstream
  `keepalive` sizing is not available (use `connection_pool { keepalive_size ...; }`).

Examples that are valid in V1:

```nginx
location = /ready { ... }
location /api/:id { ... }
location /files/*tail { ... }
location /api/ { ... }
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
location @named { }
rewrite ^/a/(.*)$ /b/$1 last;
```

## Request Routing Semantics

- Requests are first matched by the selected `http`-level listener.
- Non-TLS listeners accept HTTP/1.1 only.
- TLS listeners terminate HTTPS and negotiate HTTP/1.1 or HTTP/2 automatically.
- TLS listeners configured with `http3` also accept HTTP/3 over QUIC on UDP.
- Within a listener, `server_name` is matched by exact hostname after stripping
  any `:port` suffix from `Host`.
- `location` matching uses only the URI path. Query strings are not part of the
  route key and are proxied upstream unchanged.
- `location = /path` matches the path exactly.
- Non-exact `location` patterns are compiled into `RoutePathMatcher` routes:
  `/api/:id` matches one path segment, `/files/*tail` matches a tail segment,
  and `/` behaves like a catch-all.
- A plain non-exact pattern without `:` or `*` is treated as prefix shorthand.
  For example, `location /api/ { ... }` is compiled like `/api/*`.
- When multiple non-exact routes overlap, matcher priority follows
  `static > :placeholder > *wildcard`.
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
  static target host from `proxy_pass`, or the named upstream's configured name
  for `proxy_pass http://<upstream_name>;`.
- No automatic variable-based forwarding headers are added.

## Error Mapping

- Upstream connect failure: `502 Bad Gateway`
- Upstream premature close: `502 Bad Gateway`
- Upstream connect timeout: `504 Gateway Timeout`
- Upstream read timeout: `504 Gateway Timeout`
- Upstream send timeout: `504 Gateway Timeout`
- No matching route: `404 Not Found`

## Runtime Notes

- The runtime config should be immutable after load.
- Config parsing and semantic validation must be separate from request handling.
- Listener configuration should be centralized at the `http` level so the app
  can reuse a small number of `HttpServer` instances.
- Each worker event loop should own its local upstream client state.
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

- `conf/lite_nginx.conf`: bundled sample config.
- `src/app/`: process entry and CLI/config loading flow.
- `src/config/`: lexer, parser, directive AST, semantic validation, file loader.
- `src/runtime/`: immutable compiled config, listener/server startup, route
  dispatch.
- `src/proxy/`: proxy request forwarding, response relay, header filtering, and
  error mapping.
- `src/upstream/`: upstream registry, peer selection, and keepalive pool access.
- `tests/`: config/runtime/proxy tests.

## Module Responsibilities

- `app/`: process startup, signal handling, config loading, server lifecycle.
- `config/`: lexer, parser, directive AST, semantic validation.
- `runtime/`: immutable compiled config, server matcher, location matcher.
- `proxy/`: request forwarding, response relay, header filtering, error mapping.
- `upstream/`: upstream registry, peer selection, and connection pool access.

## Current Example

```nginx
worker_processes 1;

http {
    listen 8080;
    listen 8443 ssl http3;

    upstream backend {
        server 127.0.0.1:9001;
        server 127.0.0.1:9002;
        keepalive 32;
    }

    server {
        server_name localhost;

        location = /ready {
            proxy_pass http://127.0.0.1:9009;
        }

        location /api/:id {
            proxy_pass http://backend;
            proxy_set_header Host backend.internal;
        }

        location /files/*tail {
            proxy_pass http://backend;
            proxy_buffering off;
        }
    }
}
```
