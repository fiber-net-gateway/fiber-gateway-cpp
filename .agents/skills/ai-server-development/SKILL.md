---
name: ai-server-development
description: Design, develop, review, debug, or test the C++ LLM proxy under apps/ai-server in fiber-gateway-cpp while preserving the business behavior of /home/dear/CLionProjects/ploto-gateway/ploto-llm. Use for AI server architecture, BT1 authentication, model authorization, provider selection and token failover, OpenAI or Anthropic proxying, protocol bridging, SSE streaming, request mutation, Nacos-backed configuration, token rate limiting, audit, and related interoperability work; consult the Java implementation, its tests, and ploto-gateway/documents, then adapt the design to this repository's C++23 fiber runtime, ownership, error, and performance constraints.
---

# AI Server Development

Preserve `ploto-llm` business semantics without porting its Java object model mechanically. Treat the live Java code and tests as executable behavior evidence, use `documents/` to recover intended contracts and edge cases, and design the C++ implementation around this repository's existing runtime.

## Establish the Reference Baseline

1. Work from `/home/dear/CLionProjects/fiber-gateway-cpp` and inspect the current worktree before making changes.
2. Verify `/home/dear/CLionProjects/ploto-gateway/ploto-llm` and `/home/dear/CLionProjects/ploto-gateway/documents` exist. Report a missing or inaccessible reference instead of substituting a different checkout.
3. Read [references/reference-map.md](references/reference-map.md) and load only the sections relevant to the task.
4. Reinspect the live Java sources, tests, and documents for every task. Do not rely on remembered behavior because the Java project can change independently.
5. Follow the complete Java request path from ingress through authentication, authorization, routing, provider execution, streaming or response completion, accounting, and cleanup as applicable. Record both success and failure paths.

Use this evidence priority:

1. Follow the user's explicit requirement.
2. Use current Java production code plus focused Java tests as the compatibility baseline.
3. Use `documents/` for intended contracts, wire formats, configuration semantics, and planned boundaries.
4. Use current C++ code for framework, lifecycle, memory, and error-handling conventions.

If these sources disagree, show the exact conflict and its observable impact. Do not silently choose one or encode an accidental Java-framework behavior as a business requirement.

## Separate Behavior from Implementation Shape

Extract a behavioral contract before designing or editing C++ code:

- Inputs, defaults, validation, and configuration refresh semantics.
- Authentication and authorization decisions and externally visible errors.
- Provider and API-token ordering, unavailable-state scope and expiry, retry gates, fallback boundaries, and attempt limits.
- Request and response transformations, unknown-field handling, and protocol-specific errors.
- Streaming milestones: response commitment, SSE framing, first-token handling, cancellation, partial output, and retry prohibition after output starts.
- Token accounting, rate-limit admission and settlement, audit fields, and cleanup on every terminal path.
- Ownership, concurrency, snapshot publication, shutdown, and reconnect behavior.

For a non-trivial feature, maintain a compact parity matrix with `behavior`, `Java evidence`, `C++ design`, `intentional divergence`, and `test` columns. Resolve unexplained divergences before calling the work complete.

## Design for This Repository

- Place the runnable application and app-local code under `apps/ai-server`; keep its CMake target names consistent with existing underscore-style targets. Reinspect `apps/README.md` and neighboring apps before creating layout or targets.
- Reuse `fiber_lib` facilities before introducing app-local substitutes. Inspect `src/http`, `src/async`, `src/event`, `src/net`, `src/common/mem`, and `src/common/json` according to the task.
- Use explicit construction and a clear lifetime root instead of reproducing Guice injector graphs. Distinguish process-wide, project/config-snapshot, connection, and request ownership.
- Keep event-loop-owned mutable state on its owner loop. Publish immutable or explicitly synchronized snapshots when configuration must cross loops.
- Express asynchronous work with the repository's coroutine and callback conventions. Express failures with existing result/status types; do not use C++ exceptions.
- Use `mem::IoBuf` and `mem::IoBufChain` for request, response, and SSE data flow where practical. Avoid flattening or reparsing streaming bodies without a demonstrated need.
- Preserve backpressure and cancellation end to end. Stop provider work promptly when the downstream client closes.
- Keep hot paths allocation-conscious. Avoid defaulting to `std::string`, `std::vector`, or `std::function` in per-token or per-chunk paths; justify unavoidable ownership and allocation.
- Prefer `event::EventLoop::current().now()` for request-path time and expiry decisions.
- Establish required invariants at construction or initialization boundaries; avoid steady-state nullable checks when ownership can make the invariant explicit.
- Reuse `apps/nacos`, `apps/prometheus`, or `apps/cat` through their public APIs when the feature requires them. Invoke their applicable repository skills before changing those modules or relying on external fixtures.

Translate Java mechanisms by responsibility, not by class count. For example, map dependency injection to explicit ownership, exception control flow to status results, reactive streams to bounded coroutine-driven streaming, and garbage-collected DTO graphs to request-scoped or snapshot-owned storage.

## Work in Bounded Milestones

1. For an audit, behavior question, bug report, or design request, inspect and explain the evidence before editing. Honor any request to avoid code changes.
2. Define the smallest vertical behavior slice and its parity tests. Avoid scaffolding unrelated future layers.
3. Keep business policy separable from HTTP transport so routing, authorization, failure classification, and transformations can be tested without live sockets.
4. Implement the slice with one clear owner for each mutable state machine.
5. Add focused C++ tests for the success path, each compatibility edge, cancellation or shutdown, and bounded retry behavior where relevant.
6. Run the narrowest affected target first, then the relevant broader CTest suite. Run focused Java tests only when needed to confirm ambiguous reference behavior; do not modify the Java project unless the user explicitly asks.
7. Run `./format_code.sh` once near completion, then inspect `git diff --check` and the final diff. Preserve unrelated user changes.
8. Update app documentation when configuration, wire behavior, lifecycle, or operational commands change.

## Validate Observable Parity

Prefer contract and differential tests over class-for-class comparisons:

- Feed equivalent configuration and requests to the Java and C++ behavior surfaces when an interoperable harness is available.
- Compare status, headers, body or SSE event sequence, provider attempt order, retry stopping point, error payload, token usage, and audit outcome.
- Make time, hashing keys, runtime availability, and provider responses deterministic in unit tests.
- Verify that a request cannot retry after downstream response commitment and that every retry chain is bounded by its resolved attempt plan.
- Test configuration replacement and object destruction so obsolete snapshots, subscriptions, provider state, and in-flight requests have explicit lifetimes.

Report what was matched, what intentionally differs for the C++ runtime, and what remains unverified. Do not claim parity from compilation or happy-path tests alone.
