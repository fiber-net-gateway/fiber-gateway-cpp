# AI Server

## Overview

`ai-server` is the C++ LLM proxy application. Nacos configuration, HTTP traffic, and CAT transport have separate
EventLoop ownership. The process subscribes to the Java-compatible LLM configuration set and publishes serving
snapshots to HTTP workers. The HTTP listener is not bound until the first complete configuration snapshot is installed
on every worker. LLM request authentication, routing, and provider proxying are not exposed yet.

## Build

Configure applications and build the executable:

```bash
cmake -S . -B build -DFIBER_BUILD_APPS=ON
cmake --build build --target fiber_app_ai_server
```

The binary is written to `build/apps/ai-server`.

## Run

The process loads all startup settings from a dotenv-style file. With no argument it reads `ai-server.env` from the
current working directory; an optional positional argument selects another `xxx.env` file.

```bash
cp apps/ai-server/ai-server.env.example ai-server.env
./build/apps/ai-server
./build/apps/ai-server /etc/ai-server/production.env
```

The parser accepts blank lines, `#` comments, optional `export`, single- or double-quoted values, and inline comments
after unquoted whitespace. Duplicate and unknown keys are rejected so misspelled settings fail during startup.

`NACOS_SERVER_ADDRESSES` is required and accepts comma-separated IPv4 or IPv6 literals. DNS names are not accepted
because the current public `fiber::nacos` configuration API owns resolved IP addresses. `NACOS_USERNAME` and
`NACOS_PASSWORD` must either both be empty or both be set. Supported settings are:

- `AI_SERVER_LISTEN_ADDRESS` (default `0.0.0.0`)
- `AI_SERVER_LISTEN_PORT` (default `8080`; port `0` selects an unused local port)
- `AI_SERVER_INITIAL_CONFIG_TIMEOUT_MS` (default `60000`; `0` waits indefinitely without binding)
- `NACOS_SERVER_ADDRESSES` (required)
- `NACOS_HTTP_PORT` (default `8848`)
- `NACOS_GRPC_PORT` (default `9848`)
- `NACOS_NAMESPACE_ID`, `NACOS_TENANT`, `NACOS_USERNAME`, and `NACOS_PASSWORD`
- `NACOS_CONTEXT_PATH` (default `/nacos`)
- `NACOS_CLIENT_VERSION` (default `fiber-nacos/1.0`)

Check liveness and readiness with:

```bash
curl -i http://127.0.0.1:8080/health
curl -i http://127.0.0.1:8080/ready
```

Before initial configuration synchronization completes, the configured port is not open, so neither probe is
reachable. Once serving starts, `GET /health` returns `200` with `{"status":"ok"}` and `GET /ready` returns `200` with
`{"status":"ready"}`. Other methods on either probe return `405`; all other paths return `404` until their business
handlers are implemented.

## Nacos configuration

All LLM JSON uses Nacos group `LLM-SERVER`. The manager keeps two fixed subscriptions:

- `ploto.ai-llm.auth.bt1.keys`
- `ploto.ai-llm.models`

The model table drives subscriptions for its referenced configuration:

- `ploto.ai-llm.provider.<provider-name>`
- `ploto.ai-llm.user-group.<group-name>`

The JSON envelope, aliases, defaults, validation rules, and non-monotonic `version` behavior follow
`docs/java-ploto-llm-business-flow-and-config.md`. Unknown fields are ignored. An invalid first value remains
unavailable; an invalid later value is rejected while the last valid snapshot stays active. Provider changes rebuild
the immutable project snapshot. User-group changes update stable group state in place so later authorization checks
see new membership without rebuilding the project. Unreferenced dynamic subscriptions are released after a valid
model-table switch.

Initial startup deliberately uses a stricter gate than Java runtime refresh behavior. The first serving snapshot
requires valid BT1 keys, a valid model table, and a valid first snapshot for every Provider and user group referenced
by that model table. `NotFound`, invalid JSON, and never-synced subscriptions keep the listener closed. A later valid
value can complete startup. After the listener is open, invalid updates and Nacos disconnects retain the last valid
snapshot and do not close or rebind the port.

Configuration rejection logs include only Data ID, MD5, field, offset, and the validation message. Provider API token
values and BT1 secrets are never logged.

## EventLoop ownership

- The main EventLoop owns the listener and accepts connections.
- The HTTP worker EventLoopGroup contains one loop per CPU in the process affinity mask. Accepted connections are
  distributed round-robin. Each worker keeps a loop-local pointer to the latest serving snapshot, so requests do not
  lock the cross-loop publication watch.
- A single-loop Nacos EventLoopGroup owns `NacosClient`, `ConfigService`, subscriptions, parsing, and snapshot
  publication.
- A separate single-loop CAT EventLoopGroup reserves CAT sender ownership. The current ai-server milestone does not
  create a `CatClient`; no CAT endpoint is invented by startup defaults.

The process handles `SIGINT` and `SIGTERM`, including while it is waiting for initial configuration. Shutdown first
drains HTTP connections and stops worker snapshot watchers, then stops the configuration manager, the Nacos
configuration service, and finally the Nacos authentication client. EventLoop groups stop only after their owned
resources have completed shutdown.

## Linked libraries

- `fiber::nacos`: active authentication and configuration transport.
- `fiber::cat`: linked for future request and provider-call tracing; its sender loop is already isolated.
- `fiber::prometheus`: linked for future fixed-schema service metrics and text collection.

## Layout

- `src/AiServer.*`: HTTP server ownership and request dispatch boundary.
- `src/AiServerConfig.*`: dotenv parsing plus HTTP and Nacos startup validation.
- `src/AiServerRuntime.*`: delayed listener binding, cross-loop Nacos lifecycle, and ordered shutdown ownership.
- `src/config/LlmConfigCodec.*`: Java-compatible JSON parsing and validation.
- `src/config/LlmConfigManager.*`: fixed/dynamic subscriptions and last-known-good update policy.
- `src/config/LlmConfigSnapshot.*`: immutable BT1, Provider, model-project, and user-group snapshot types.
- `src/main.cpp`: config path, signal handling, CPU-sized HTTP workers, and Nacos/CAT EventLoop lifetime.

## Notes

- This milestone serves cleartext HTTP/1.1 only.
- Request bodies are drained after handlers complete so keep-alive connections remain reusable.
- Cross-protocol bridging is intentionally out of scope. OpenAI requests only use an
  `openai-chat-completions` provider protocol, and Anthropic requests only use an `anthropic-messages` protocol.
  A provider intended to serve both endpoints must configure both protocol entries.
- No LLM endpoint claims compatibility until its Java behavior path and parity tests are implemented.
