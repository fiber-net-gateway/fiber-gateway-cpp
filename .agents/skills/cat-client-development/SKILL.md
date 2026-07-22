---
name: cat-client-development
description: Develop, review, debug, or test the native CAT client under apps/cat against the official Dianping CAT 3.0 client and server behavior, including Transaction/Event/Metric APIs, message trees and IDs, NT1/PT1 encoding, router and collector transport, sampling, aggregation, heartbeat, coroutine context propagation, lifecycle, performance, Log View, and CAT interoperability. Use when fiber2 CAT work needs CAT 3.0 compatibility analysis, official source comparison, or preparation of its sources under temp/cat.
---

# CAT Client Development

Treat official Dianping CAT `v3.0.0` as the compatibility authority for `apps/cat`. Use its C/C++ client as the emission reference and its Java client/server code as the acceptance, storage, and UI-query reference. Adapt the client semantics to fiber2's C++23 coroutine and event-loop architecture instead of mechanically porting its runtime.

The pinned baseline is commit `f875ff10b1a3f2922fef1bfca7ba34c54805b021`. Do not silently use `master`, a newer tag, or behavior remembered from a newer CAT release. Use another revision only when the user explicitly targets a different CAT server version, and label all resulting compatibility conclusions with that revision.

## Prepare the Official Source

1. Work from the repository root.
2. Run the preparation script before relying on `temp/cat`, even when the directory already exists:

   ```bash
   ./.agents/skills/cat-client-development/scripts/prepare_cat_source.sh
   ```

   With no `--ref`, the script reuses a clean cached checkout at the pinned CAT `v3.0.0` commit without network access, or fetches and detaches at `v3.0.0` when the checkout is absent or points elsewhere.
3. Only when the task explicitly targets another CAT version, pass `--ref <revision>`. The script fetches and checks out that revision after confirming the checkout is clean.
4. Record `git -C temp/cat rev-parse HEAD` in analysis or handoff notes. For the default baseline, require the exact pinned commit above.
5. Never edit `temp/cat` as part of the fiber2 implementation. It is ignored reference material. If preparation fails, report the exact Git, network, origin, dirty-tree, or layout error; do not silently substitute another CAT client.

Read [references/official-client-map.md](references/official-client-map.md) before tracing upstream code. It maps behavior to the smallest relevant source areas and calls out architectural traps.

## Analyze Before Changing Code

1. Inspect the current `apps/cat` source, CMake integration, tests, and relevant design notes. If the directory has not been created yet, state that explicitly and derive the initial boundary from adjacent `apps/` modules.
2. Define the requested contract before implementation: public API, message-tree semantics, propagation IDs, wire encoding, routing, queue policy, lifecycle, or failure handling.
3. Trace the exact official C implementation behind the C++ facade. The files under `lib/cpp/src/cppcat/` are thin wrappers; do not infer behavior from them alone.
4. For message IDs, collector decoding, storage lookup, Router compatibility, or CAT UI failures such as Log View HTTP 500, inspect the corresponding CAT 3.0 Java client/server path as well. Those paths are outside the sparse working tree; read them with `git -C temp/cat show HEAD:<path>` as listed in the reference map.
5. Separate observations from design choices. Label behavior as CAT 3.0-compatible, intentionally fiber2-specific, or still uncertain.
6. If the user requests evaluation only, stop after evidence, risks, and an implementation estimate. Do not create project code.

## Translate to fiber2

- Preserve CAT 3.0-visible semantics: Transaction nesting and completion, Event and Metric records, message/root/parent IDs, status and duration handling, router responses, sampling/block rules, encoder framing, reconnect behavior, heartbeat content, storage lookup, and Log View requirements.
- Do not use OS thread-local state for the active transaction tree. Multiple fiber2 coroutines can suspend and interleave on one event-loop thread. Keep context explicitly owned by the request/coroutine and make cross-service propagation explicit.
- Do not port CAT's dedicated pthreads, blocking initialization, private socket event loop, blocking joins, or lock-heavy global maps. Use repository-native `EventLoop`, async tasks, DNS, HTTP/1, `TcpStream`, timers, and lifecycle patterns.
- Keep mutable transport and queue state owner-loop local. Make `start`, shutdown, draining, reconnect, and close-wait behavior explicit and asynchronous.
- Bound queues and buffers. Define overload behavior and expose dropped-message or encoding-failure counters; never allow observability traffic to apply unbounded memory pressure to application traffic.
- Preserve the hot record path. Avoid `std::function`, repeated `std::string`/`std::vector` growth, global locks, and per-field heap churn unless measurements justify them. Prefer reusable buffers, fixed or intrusive structures, and `IoBuf`/`IoBufChain` where appropriate.
- Follow repository rules: no exceptions, explicit invariants, and `fiber::event::EventLoop::current().now()` for request-path time.
- Use `apps/prometheus` only as a repository-native reference for metric storage, sharding, or benchmarks. CAT protocol and reporting behavior remain defined by the pinned official CAT 3.0 client and server.

## Verify Changes

Match validation to the changed layer:

- Add focused unit tests for message-tree state, IDs, escaping, duration/status behavior, sampling, and queue limits.
- Add CAT 3.0 receiver-contract tests for fields parsed more narrowly by the Java server than they can be represented locally. In particular, keep message-ID hour/index fields non-negative and within Java `int`, and cover Log View-compatible parsing.
- Add golden-byte tests for each implemented NT1/PT1 encoder path, derived from the checked-out upstream revision.
- Exercise routing and collector behavior with deterministic fake HTTP and TCP peers before requiring a live CAT server. Cover partial writes, reconnects, malformed router replies, backpressure, and shutdown with queued data.
- Add coroutine interleaving tests that suspend nested transactions from multiple requests on the same event loop; these guard against accidental TLS-style context corruption.
- Run the `apps/cat` test target and relevant CTest selection shown by the current CMake files, then broaden validation in proportion to the change.
- For implementation tasks, run `./format_code.sh` once after code and tests are complete. Inspect the final diff and report exact build/test commands.
