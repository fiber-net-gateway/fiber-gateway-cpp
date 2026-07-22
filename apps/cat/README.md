# CAT Client Library

## Overview

`apps/cat` is the reusable native CAT client library for applications under `apps/`. Consumers link the `fiber::cat`
target and include headers from `fiber/cat/`.

The implementation provides owner-EventLoop-local recording, automatic CAT message IDs, explicit cross-service
propagation, NT1/PT1 encoding, bounded sampling aggregates, Count/Duration Metric transport, startup/heartbeat system
messages, Router discovery, and a non-coroutine raw collector sender. All growing recording, aggregation, heartbeat,
Router-response, and outbound-frame state has explicit limits.

## Build and test

```bash
cmake -S . -B build -DFIBER_BUILD_APPS=ON -DFIBER_BUILD_TESTS=ON
cmake --build build --target fiber_cat_tests
ctest --test-dir build -R '^(Cat|CatClientConfig)'
```

## Client and trace lifecycle

Create and start `CatClient` on its sender EventLoop. Configuration may contain CAT router HTTP endpoints, direct
bootstrap collectors, or both. A hostname router requires an `AddressResolver` bound to the sender loop; literal IP
routers and collectors do not. A supplied resolver must outlive the client.

```cpp
fiber::cat::CatClientConfigParams params{
        .app_key = "checkout",
        .hostname = "host-a",
        .ip = "10.0.0.8",
};
params.routers.push_back({.host = "10.0.0.10", .port = 8080});

auto config = fiber::cat::CatClientConfig::create(std::move(params));
if (config) {
    auto client = fiber::cat::CatClient::create(loop, std::move(*config));
    if (client && (*client)->start()) {
        auto trace_result = fiber::cat::MessageTrace::create(**client);
        if (trace_result) {
            auto trace = std::move(*trace_result);
            auto root_result = trace.create_transaction("URL", "/orders");
            if (root_result) {
                auto root = std::move(*root_result);
                root.log_event("Cache", "miss");
                root.complete();
            }
        }
    }
}
```

`MessageTrace`, `Transaction`, and `Event` are move-only one-pointer handles. A trace permits exactly one root. The
final open message completion synchronously encodes the tree, submits the independently owned frame to the client,
destroys `MessageTraceData`, and resets the trace pool. A still-live public trace handle then becomes an inert shell:
`valid()` is false and no later root can be created.

Call `co_await client->shutdown()` before stopping the sender EventLoop. Shutdown closes frame admission, waits for
submitters that already reserved capacity, crosses a complete sender-loop Notify phase, drains the connected sender up
to `shutdown_drain_timeout`, drops the remainder, closes the collector socket, and completes in `Stopped` state.

Every producer EventLoop that created a trace or client-bound Metric owns one aggregation shard. Destroy all live
trace/Metric handles on that loop, then call `co_await client->detach_current_event_loop()` before stopping the producer
loop. Detach performs a final flush, accounts for any residual drop, unregisters the shard, and crosses a complete
Notify phase. `shutdown()` automatically performs this detach for a shard owned by the sender loop.

`CatClientOptions` selects `Nt1` (the default) or `Pt1`, bounds normal/problem/system queues independently, controls
sampling and aggregation cardinality, and configures Router, collector, heartbeat, and shutdown timeouts.

## CAT propagation context

An empty `MessageTraceContext::message_id` is filled automatically using the official visible
`domain-ipHex-hour-sequence` structure. The owning `PropagationContext` can safely outlive the trace:

For CAT 3.0 Log View compatibility, `hour` and `sequence` are always non-negative Java `int` values. The sequence uses
a process-specific bounded starting point, resets when the wall-clock hour advances, retains the latest hour across a
clock rollback, and fails explicitly on exhaustion instead of wrapping to a duplicate ID.

```cpp
auto current = trace.propagation_context();
if (current) {
    auto inventory = client->create_remote_context(*current, "inventory");
    // Map the four propagation fields to approved outbound headers/metadata.
}
```

Inbound propagation is passed explicitly to `MessageTrace::create(client, context)`. No OS TLS or implicit current
transaction stack is used. IDs and session tokens are validated and copied into trace-owned storage.

## Service context

An active `MessageTrace` owns an optional case-sensitive key/value context for request-local service propagation. The
table is a fiber2 extension and is not encoded into CAT NT1/PT1 messages. HTTP or gRPC integration may populate it from
approved inbound metadata and synchronously copy it into an outbound request:

```cpp
trace.put_context("tenant", "blue");
auto tenant = trace.get_context("tenant");
trace.for_each_context([](std::string_view key, std::string_view value) noexcept {
    // Copy key and value into request-owned header or metadata storage.
    return true;
});
trace.remove_context("tenant");
```

Keys must be non-empty and are matched byte-for-byte. Empty values are valid. Returned `string_view` values and visitor
arguments borrow trace-owned storage; callers must copy them before retaining them asynchronously. Context access is
owner-EventLoop-local. It becomes `RecordError::Completed` after final message completion resets the trace pool.

`RecordLimits` independently bounds context entry count, key size, value size, and cumulative arena bytes. Removed or
replaced storage is not subtracted from the byte budget because `BufPool` releases memory only when the complete trace is
reset. Applications should apply an outbound propagation allowlist instead of forwarding arbitrary context to
untrusted destinations.

## Transactions and events

Create a trace root and all its child messages on one running EventLoop. Handles may live across coroutine suspension.
Parent-child relationships are explicit, so interleaved coroutines on one loop do not share an implicit transaction
stack. Standalone `Transaction::create_root()` and `Event::create_root()` remain available for recording-only use; a
sendable tree starts from `MessageTrace` bound to a running client.

```cpp
auto root_result = fiber::cat::Transaction::create_root("URL", "/orders");
if (root_result) {
    auto root = std::move(*root_result);
    root.add_data("method", "GET");

    auto sql_result = root.start_transaction("SQL", "select-order");
    if (sql_result) {
        auto sql = std::move(*sql_result);
        sql.log_event("Cache", "miss");
        sql.complete();
    }
    root.complete();
}
```

`complete()` consumes the handle: after successful completion, `valid()` is false and mutating operations return
`RecordError::Completed`. Completion without a status uses success (`"0"`). Destroying an unfinished Transaction or
Event completes it with `CAT_CLIENT_INCOMPLETE`, preventing an abandoned operation from being reported as success.
`log_error()` emits the official `Exception`/`ERROR` event form. `log_completed_transaction()` records a transaction
whose supplied duration ends at the current wall-clock time, without introducing an implicit transaction stack.

A parent may complete before children that were already created. Its internal data remains in the trace arena until
the final open child completes, but the consumed parent handle cannot add more children. The final completion destroys
the complete trace arena in one operation. Views into the internal tree are intentionally not part of the public API.

Type, name, status, message count, child count, per-message data, and total tree memory are bounded by `RecordLimits`.
Message strings and rendered data are copied into trace-owned pooled storage at record time. Transactions store child
pointers in linked fixed-capacity chunks of 16; message data is rendered into linked byte chunks.

## Encoding, sampling, and aggregation

The private NT1 encoder follows the official CAT C client field order and framing: a four-byte big-endian payload
length, the `NT1` header, and the depth-first message body. PT1 uses the same frame prefix and the official text header
plus `t/T/A/E/M/H` line forms. Both encoders perform a counting pass followed by one exact `IoBuf` allocation.

An internally core-bound trace synchronously encodes when its final open message completes. The core receives an
independently owned buffer, after which the complete trace arena is immediately destroyed. Encoding failures are
reported to the core without submitting a partial frame.

Sampling is decided only after the tree freezes. Problem, incomplete, and truncated trees bypass sampling and enter the
bounded high-priority path. Ordinary trees not selected for detailed reporting, or rejected by the reserved normal
queue budget, update the owner-loop Transaction/Event aggregate without allocating a detailed frame. Aggregate keys
are bounded by count, key length, duration-bucket count, and total shard bytes. System aggregates do not recurse through
normal sampling.

When a tree hits a recording limit, its root is marked as a problem and the encoder appends a bounded
`CatClient.Truncated=count:...,bytes:...,reason:...` marker without another trace-pool allocation.

## Router and collector transport

The control plane is coroutine-based. It periodically fetches
`/cat/s/router?op=json&domain=...&ip=...&hostname=...`, resolves router hostnames, parses bounded
`kvs.routers/sample/block` responses, rotates collectors, and reconnects with capped exponential backoff. A failed
refresh retains the last usable collector set.

The data plane does not use a sender coroutine or a CAT-private cross-thread queue. Each finalized trace reserves a
packed outstanding message/byte budget and owns an `OutboundFrame` with an intrusive `NotifyEntry`. Cross-loop
submissions enter the sender EventLoop's existing MPSC Notify queue directly; same-loop submissions append directly to
the owner-loop FIFO. Notify callbacks only collect complete frames, and one coalesced `DeferEntry` batches up to 16 NT1
frames for `try_writev`. `WouldBlock` arms a writable callback and a write deadline; fairness limits defer continued
pumping through the local callback. Frames are concatenated on the raw TCP stream. If a connection fails after writing
only a prefix of a frame, that partial frame is dropped rather than resumed on a new collector connection.

Problem/system frames may overtake normal frames that have not started. Once any frame has written a prefix, it remains
pinned until its complete frame boundary, so priority cannot interleave bytes. A Router refresh keeps the current
connection when its collector remains present; removal switches a partially active connection only after that frame.

`CatClientStats::queued_messages` and `queued_bytes` report all outstanding frames from admission until complete send
or explicit drop. They include EventLoop Notify entries, owner-loop FIFO entries, writable waits, and partially written
frames rather than only the physical length of one queue. System frames have an additional independent message/byte
budget.

## Metrics

Metrics are pre-bound to one name and one kind. `Metric::create_count()` accumulates signed quantities;
`Metric::create_duration()` accumulates observation count and total milliseconds. Recording and snapshots must occur
on the EventLoop that created the Metric.

```cpp
auto latency_result = fiber::cat::Metric::create_duration("request.latency");
if (latency_result) {
    auto latency = std::move(*latency_result);
    latency.record_duration(std::chrono::milliseconds(12));
    auto snapshot = latency.snapshot_and_reset();
}
```

The overloads taking `CatClient&` are automatically reported through that loop's bounded shard as
`System/MetricAggregator` with `M` records. Standalone Metrics remain snapshot-only; snapshot/reset operations are
intentionally rejected for client-bound Metrics. A standalone snapshot's name view remains valid only while its Metric
remains alive.

## Heartbeat and statistics

When enabled, the sender loop emits one delayed `System/Reboot` tree and periodic `System/Status` trees containing an
`H Heartbeat/<client-ip>` record. Heartbeat data is bounded by field and byte limits and includes identity, version,
process/uptime, active shard count, queue counters, Router/sample/block state, collector state, and client failure
counters. Only one heartbeat may be outstanding.

`CatClientStats` separates record truncation, sampling-to-aggregate conversion, aggregate retry/drop, encode failures,
normal/system admission, Router changes, connect failures, `WouldBlock`, partial-frame loss, Metric activity, and
heartbeat submission/delivery. Values are atomically readable from any thread.

## Layout

- `include/fiber/cat/`: public client/configuration API, single-pointer recording handles, and value types.
- `src/`: private trace/message layout, NT1/PT1 encoders, bounded aggregation, Router parser, sender, connection manager,
  message-ID generator, heartbeat builder, and metric implementations.
- `tests/`: lifecycle, limits, chunk boundaries, routing, cross-loop collector transport, metrics, and coroutine
  interleaving tests.
