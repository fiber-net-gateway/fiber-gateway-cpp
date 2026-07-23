# AI Server

## Overview

`ai-server` is the C++ LLM proxy application. This initial milestone establishes the application target, links the
repository's Nacos, CAT, and Prometheus libraries, and starts an HTTP/1.1 server with a health endpoint. LLM routing and
provider proxy behavior will be added in bounded compatibility milestones against `ploto-gateway/ploto-llm`.

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
- `NACOS_SERVER_ADDRESSES` (required)
- `NACOS_HTTP_PORT` (default `8848`)
- `NACOS_GRPC_PORT` (default `9848`)
- `NACOS_NAMESPACE_ID`, `NACOS_TENANT`, `NACOS_USERNAME`, and `NACOS_PASSWORD`
- `NACOS_CONTEXT_PATH` (default `/nacos`)
- `NACOS_CLIENT_VERSION` (default `fiber-nacos/1.0`)

Check readiness with:

```bash
curl -i http://127.0.0.1:8080/health
```

`GET /health` returns `200` with `{"status":"ok"}`. Other methods on `/health` return `405`; all other paths return
`404` until their business handlers are implemented.

## Linked libraries

- `fiber::nacos`: future dynamic project, provider, authentication, and routing configuration.
- `fiber::cat`: future request and provider-call tracing.
- `fiber::prometheus`: future fixed-schema service metrics and text collection.

The libraries are linked, and the Nacos client configuration is parsed and validated at startup. The Nacos, CAT, and
Prometheus clients are not constructed yet; their EventLoop ownership, startup, and shutdown ordering belongs to later
application-lifecycle milestones.

## Layout

- `src/AiServer.*`: HTTP server ownership and request dispatch boundary.
- `src/AiServerConfig.*`: dotenv parsing plus HTTP and Nacos startup validation.
- `src/main.cpp`: config path, listener binding, and EventLoop lifetime.

## Notes

- This milestone serves cleartext HTTP/1.1 only.
- Request bodies are drained after handlers complete so keep-alive connections remain reusable.
- Cross-protocol bridging is intentionally out of scope. OpenAI requests only use an
  `openai-chat-completions` provider protocol, and Anthropic requests only use an `anthropic-messages` protocol.
  A provider intended to serve both endpoints must configure both protocol entries.
- No LLM endpoint claims compatibility until its Java behavior path and parity tests are implemented.
