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
- `rewrite`, `if`, regex `location`, or dynamic config reload.
- DNS-based upstream resolution.
- Cache, gzip, upstream HTTP/2, or active health checks.
- Full logging subsystem compatibility with nginx.

## V1 Functional Scope

- Accept downstream HTTP/1.1 requests on non-TLS listeners.
- Accept downstream HTTPS requests on TLS listeners.
- Negotiate HTTP/1.1 or HTTP/2 automatically on TLS listeners via ALPN.
- Accept downstream HTTP/3 on TLS listeners configured with `http3`.
- Proxy to upstream HTTP/1.1 backends.
- Proxy WebSocket tunnels automatically through `proxy_pass`, accepting HTTP/1.1
  Upgrade and HTTP/2 or HTTP/3 Extended CONNECT downstream requests.
- Support multiple `server` blocks.
- Support shared `http`-level listeners reused by all `server` blocks.
- Support `server_name` exact matching.
- Support `location` patterns through `RoutePathMatcher`: a bare pattern matches
  exactly that path, `:name` captures one segment, and `*name`/`*` is a trailing
  wildcard.
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

### File Paths and `include`

Paths referenced by a config directive - `script_file`, `certificate`,
`certificate_key`, and the target of `include` - are resolved as:

- **Absolute** paths (leading `/`) are used as-is.
- **Relative** paths are resolved against the directory of the config file that
  contains the directive, **not** the process working directory. The bundled
  `conf/lite_nginx.conf` therefore uses paths like `scripts/health.js` (next to
  the conf) regardless of where the binary is launched from.

`include <path>;` splices another config file's top-level directives in place of
the directive, anywhere a directive list is valid (top-level, or inside `http`,
`server`, `location`, `upstream`, `connection_pool`). An included file's own
paths and nested `include`s resolve relative to the included file, and include
cycles are detected and rejected.

## Supported Directives In V1

Top level:

- `worker_processes <n>;`
- `logging { ... }` - configure console/file appenders and hierarchical loggers.
- `http { ... }`
- `include <path>;` - splice another config file's top-level directives here (valid
  in any directive-list context: top-level, `http`, `server`, `location`, `upstream`,
  `connection_pool`). See "File Paths and `include`" above.

`http` block:

- `access_log <logger-name> "<script-template>";` or `access_log off;` - select the logger
  category and synchronous message template; inherited by `server` and `location`.
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
  back to transient connections), `keepalive_timeout <duration>;`, and `steal on|off|auto;`
  (cross-loop idle-connection sharing: `auto` = on when `worker_processes > 1`). `steal on` uses
  one pool whose idle connections can be borrowed across worker loops; `steal off` gives each
  worker loop its own local pool (no cross-loop reuse). `max_idle_total <n>;` caps the total
  idle connections across all peer groups in one pool (0 or omitted => `keepalive_size * 64`);
  `initial_group_capacity <n>;` seeds the per-group hash table (0 or omitted => 16).
- `server { ... }`

`upstream` block:

- `server [<scheme>://]<host:port> [weight=<n>];` — `scheme` is `http://` (default, may be
  omitted) or `https://`; it selects TLS for the peer and is baked into the pool key. `host` may
  be an IP literal (config-time dial target, no DNS) or a hostname (resolved via DNS at connect
  time on the worker loop that needs a fresh connection; the pooled identity stays the host name).
  `weight` drives smooth weighted round-robin peer selection (default 1). Upstreams carry only
  peers + weight + scheme; pool sizing lives in `connection_pool`.
- `connect_timeout <duration>;`
- `read_timeout <duration>;`
- `send_timeout <duration>;`

`server` block:

- `access_log <logger-name> "<script-template>";` or `access_log off;`
- `server_name <name> [name ...];`
- `certificate <path>;`
- `certificate_key <path>;`
- `location <pattern> { ... }`
- `proxy_connect_timeout <duration>;`
- `proxy_read_timeout <duration>;`
- `proxy_send_timeout <duration>;`
- `proxy_set_header <name> <literal>;`

`location` block:

- `access_log <logger-name> "<script-template>";` or `access_log off;`
- `proxy_pass http://<upstream_name>;`
- `proxy_pass http://<host:port>;`
- `proxy_connect_timeout <duration>;`
- `proxy_read_timeout <duration>;`
- `proxy_send_timeout <duration>;`
- `proxy_set_header <name> <literal>;`
- `proxy_buffering off;`
- `script_file <path>;` - handle the request with a compiled script instead of proxying
  (mutually exclusive with `proxy_pass`).

## Logging

When `logging` is omitted, operational logs go to stderr and access logging is off by default.
File paths are resolved relative to the file containing the directive. `--check-config` validates
logging syntax and references without opening or creating the configured files.

```nginx
logging {
    appender access_file {
        type file;
        path logs/access.log;
        mode 0644;
        buffer_size 64k;
        flush_interval 200ms;
        rotate_size 256m;
        archive_name "{base}.{utc}.{seq}";
        rotate_keep 14;
        min_level info;
        max_level info;
    }

    appender stderr {
        type console;
        stream stderr;
        min_level warn;
    }

    logger lite_nginx.access {
        level info;
        appender access_file;
        additive off;
    }

    root_logger {
        level info;
        appender stderr;
    }
}
```

An `appender` supports `type file|console`, `min_level`, and `max_level`. File appenders support
`path`, octal `mode`, and an optional `buffer_size` + `flush_interval` pair. Size-based rotation is
enabled by configuring `rotate_size`, `archive_name`, and `rotate_keep` together. Archive names are
created next to the active file; the pattern must contain `{base}` and `{seq}`, and may contain
`{utc}` for a `YYYYMMDDTHHMMSSZ` timestamp. `{seq}` is a monotonically increasing, zero-padded
sequence. Console appenders support `stream stdout|stderr`. A `logger` supports `level`,
`verbosity`, repeated `appender` references, and `additive on|off`; `root_logger` supports `level`,
`verbosity`, and appenders.

`access_log` takes a logger category followed by a script template string. For example:

```nginx
access_log lite_nginx.access "request_id=${$access.request_id} remote_addr=${$conn.remote_addr} method=${$req.method} path=${$req.path} status=${$access.status}";
```

The category is resolved through the normal hierarchical logging configuration. The template is
compiled while the runtime configuration is built and must be synchronous; use of an async
function such as `req.readJson()` rejects the configuration. It can use the regular request
constants plus `$conn.remote_addr`, `$conn.remote_port`, `$conn.http_version`, `$conn.scheme`, and
`$conn.tls`; access-only fields are `$access.request_id`, `$access.server`, `$access.location`,
`$access.status`, `$access.body_bytes_sent`, `$access.request_time_us`, and `$access.outcome`.
Upstream fields are `$upstream.host`, `$upstream.port`, `$upstream.status`, `$upstream.time_us`,
and `$upstream.error`, and resolve to `null` when no upstream value is available. `access_log`
inherits from `http` to `server` to `location`; an explicit pair replaces the inherited pair and
`off` disables it.

## Scripting

A `script_file` location runs an embedded JS-like script per request. The script reaches the
inbound request and the outbound response through host functions and can issue upstream HTTP
calls. See `conf/scripts/` for examples (`echo.js`, `inspect.js`, `vars.js`, `proxy.js`).

Request-side functions (sync unless noted): `req.getHeader([name])`, `req.getQuery([name])`,
`req.getUri`, `req.getPath`, `req.getQueryStr`, `req.getMethod`, `req.getCookie([name])`,
`req.readJson` (async), `req.readBinary` (async), `req.discardBody` (async).

Response-side functions: `resp.setHeader(name, value)`, `resp.addHeader(name, value)`,
`resp.sendJson(status, body)` (async), `resp.send(status[, body])` (async), `resp.addCookie(obj)`.

Route-variable constants (`$namespace.key`, resolved at compile time and read from the request
at runtime -- see `conf/scripts/vars.js`):

- `$path.<name>` - a path variable captured by the location's pattern (e.g. `/api/:id` makes
  `$path.id` available). Referencing a name the pattern does not capture is a compile-time
  error (the script fails to load).
- `$query.<key>` - a query parameter (case-sensitive).
- `$header.<key>` - a request header, matched case-insensitively with `-` folded to `_`
  (e.g. `$header.x_forwarded_for` reads `X-Forwarded-For`).
- `$cookie.<key>` - a request cookie, same normalization as `$header`.
- `$req.<field>` - one of `uri` / `method` / `path` / `query` (fixed set; unknown = compile
  error). `query` is the raw query string (empty when absent).
- `$conn.<field>` - one of `remote_addr` / `remote_port` / `http_version` / `scheme` / `tls`.
  These values describe the accepted downstream connection and are also available to access-log
  templates.

Absent `$query`/`$header`/`$cookie` values resolve to `null` (not an error). `$path.<name>`
is always present for a matched route (the pattern captured it).

Upstream HTTP (async, require a `connection_pool` block for keepalive reuse). The upstream host
is bound once at compile time with a directive; `svc.request` / `svc.proxyPass` are the only
entry points (flat `http.request` / `http.proxyPass` are not available):

```
directive svc = http "@backend";          // or http "http://1.2.3.4:8080"
let r = svc.request({path: "/items"});    // target pre-bound
let st = svc.proxyPass({});
```

- `svc.request(options)` - issue an upstream request and return
  `{status:int, headers?:object, body:binary}`. `options`: `url` (request path?query, e.g.
  `/items?q=1`; not a host) or `path` + `query` (string or object), `method`, `headers`, `body`
  (binary/string/object), `timeout` (ms), `includeHeaders`.
- `svc.proxyPass(options)` - forward the inbound request to the bound upstream and stream its
  response back to the client; returns the upstream status code. `options`: `url` or
  `method`/`path`/`query` (default to the inbound values), `headers`, `responseHeaders` (set on
  the downstream response; `null` removes), `timeout`, and `websocket`. Set `websocket: true`
  to proxy an inbound HTTP/1.1 WebSocket Upgrade or HTTP/2/3 Extended CONNECT as a bidirectional
  tunnel. WebSocket mode always uses an upstream HTTP/1.1 GET Upgrade, rejects an explicit
  non-GET `method`, and uses `timeout` as the per-operation tunnel read/write timeout. Required
  handshake fields (`Connection`, `Upgrade`, and `Sec-WebSocket-Key`/`Sec-WebSocket-Accept`) are
  reasserted after `headers`/`responseHeaders` overrides.

The directive target is either a named upstream (`@backend` / `backend`) or an ad-hoc
`http(s)://host[:port]` URL; hostnames are resolved via DNS (IP literals skip DNS).

## Explicit V1 Restrictions

- `proxy_set_header` accepts literal values or synchronous `${...}` templates.
- `proxy_pass` accepts only static targets.
- Script constants in `${...}` are supported only by directives documented as templates; nginx
  variables are not interpreted.
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
- `location` uses `RoutePathMatcher` syntax and semantics (a bare pattern matches
  exactly; `:name`/`*` are matched as written), not nginx `location` precedence rules.
- Only `http://` upstream targets are supported in V1 (for `proxy_pass`).
- Upstream peers must be configured with IP literals in the current runtime. Script
  directive `http(s)://host` targets accept hostnames (resolved via DNS) or IP literals; the
  `url` call option is the request path?query, not a host.
- `proxy_buffering` only accepts `off`.
- A single global keepalive pool is shared across all upstreams and script targets; per-upstream
  `keepalive` sizing is not available (use `connection_pool { keepalive_size ...; }`).

Examples that are valid in V1:

```nginx
location /ready { ... }
location /api/:id { ... }
location /files/*tail { ... }
location /api/* { ... }
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
- A `location` pattern is compiled into a `RoutePathMatcher` route verbatim: a bare
  pattern like `/ready` matches exactly that path; `:name` captures one path segment;
  `*name`/`*` is a trailing wildcard. `/` matches only the root path; `/*` is the
  catch-all. Prefix matching requires an explicit `*` (e.g. `/api/*`) -- a bare pattern
  is not a prefix.
- When multiple routes overlap, matcher priority follows
  `static > :placeholder > *wildcard`.
- If no location matches, the request returns `404 Not Found`.

## Proxy Semantics

- The original downstream request method is forwarded unchanged.
- The original downstream request URI is forwarded unchanged.
- Request bodies are streamed to upstream.
- Response bodies are streamed back to downstream.
- Hop-by-hop headers are filtered and never forwarded upstream or downstream.
- If `proxy_set_header` overrides a header, the configured literal value wins.
- WebSocket requests need no extra directives: HTTP/1.1 Upgrade is forwarded as an
  upstream HTTP/1.1 Upgrade, while HTTP/2 and HTTP/3 Extended CONNECT are translated
  to an upstream HTTP/1.1 Upgrade. A successful upstream `101` becomes downstream
  `101` for HTTP/1.1 and `200` for HTTP/2 or HTTP/3 before bidirectional tunnelling starts.

Headers that must be filtered as hop-by-hop:

- `connection`
- `keep-alive`
- `proxy-connection`
- `transfer-encoding`
- `upgrade`
- `te`
- `trailer`

For a WebSocket handshake, `proxy_pass` synthesizes the required upstream
`Connection: Upgrade` and `Upgrade: websocket` fields after normal hop-by-hop filtering.
For Extended CONNECT it also generates the HTTP/1.1 WebSocket key, validates the
upstream `Sec-WebSocket-Accept`, and omits HTTP/1.1-only handshake fields from the
HTTP/2 or HTTP/3 response.

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

        location /ready {
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
