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

The server listens on all IPv4 interfaces. The optional positional argument selects the port; the default is `8080`.
Port `0` asks the operating system to select an unused port and prints the selected value.

```bash
./build/apps/ai-server
./build/apps/ai-server 18080
```

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

The libraries are linked but their clients are not constructed yet. Each client requires explicit configuration,
EventLoop ownership, startup, and shutdown ordering, which belongs to later application-lifecycle milestones.

## Layout

- `src/AiServer.*`: HTTP server ownership and request dispatch boundary.
- `src/main.cpp`: process arguments, listener binding, and EventLoop lifetime.

## Notes

- This milestone serves cleartext HTTP/1.1 only.
- Request bodies are drained after handlers complete so keep-alive connections remain reusable.
- No LLM endpoint claims compatibility until its Java behavior path and parity tests are implemented.
