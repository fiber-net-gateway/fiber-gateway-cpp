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
        fiber::mem::BufPool pool;
        auto root_result = (*client)->create_isolated_transaction(pool, "URL", "/orders");
        if (root_result) {
            auto root = std::move(*root_result);
            auto trace = root.message_trace();
            root.log_event("Cache", "miss");
            root.complete();
        }
    }
}
```

`CatClientConfigParams::ip` must be a specified unicast IPv4 or IPv6 literal.
Applications may discover a default with `fiber::net::detect_local_ipv4()` at startup,
but should retain an explicit deployment override for multi-interface and
container/NAT environments. The selected CAT identity must remain stable for
the client lifetime.

`Transaction` and `Event` are move-only one-pointer handles. `MessageTrace` is a move-only non-owning view obtained
from either root type. `CatClient::create_isolated_transaction()` and `create_isolated_event()` atomically create the
trace and its only root. The caller supplies the `BufPool` and must keep it alive until every root and child handle has
finished and every trace view is no longer used. CAT allocates all trace state and recorded data from that pool, but
never resets it; the pool may therefore be shared with request state or multiple independent traces.

The final open message completion synchronously encodes the tree, submits the independently owned frame to the client,
and destroys `MessageTraceData`. Its arena memory remains reserved in the caller's pool until the caller resets or
destroys the pool. A still-live trace view becomes inert after final completion: `valid()` is false and context access
returns `RecordError::Completed`.

Call `co_await client->shutdown()` before stopping the sender EventLoop. Shutdown closes frame admission, waits for
submitters that already reserved capacity, crosses a complete sender-loop Notify phase, drains the connected sender up
to `shutdown_drain_timeout`, drops the remainder, closes the collector socket, and completes in `Stopped` state.

Every producer EventLoop that created a trace or client-bound Metric owns one aggregation shard. Destroy all live
trace/Metric handles on that loop, then call `co_await client->detach_current_event_loop()` before stopping the producer
loop. Detach performs a final flush, accounts for any residual drop, unregisters the shard, and crosses a complete
Notify phase. `shutdown()` automatically performs this detach for a shard owned by the sender loop.

`CatClientOptions` selects `Nt1` (the default) or `Pt1`, bounds normal/problem/system queues independently, controls
sampling and aggregation cardinality, and configures Router, collector, heartbeat, and shutdown timeouts. Heartbeat
system statistics are enabled by default and can be disabled independently with `enable_system_stats`.

## CAT propagation context

An empty `MessageTraceContext::message_id` is filled automatically using the official visible
`domain-ipHex-hour-sequence` structure. The owning `PropagationContext` can safely outlive the trace:

For CAT 3.0 Log View compatibility, `hour` and `sequence` are always non-negative Java `int` values. The sequence uses
a process-specific starting point no greater than `1,000,000`, retains the latest hour across a clock rollback, and
fails explicitly at CAT 3.0's `50,000,000` stored-message index limit instead of emitting an ID that Log View will
discard or wrapping to a duplicate.

```cpp
auto current = root.message_trace().propagation_context();
if (current) {
    auto inventory = client->create_remote_context(*current, "inventory");
    // Map the four propagation fields to approved outbound headers/metadata.
}
```

Inbound propagation and recording limits are passed together when the isolated root is created:

```cpp
auto root = client->create_isolated_transaction(
        pool, "URL", "/orders",
        {.limits = limits, .context = inbound_context});
```

For an owning `PropagationContext`, pass `context.view()`. No OS TLS or implicit current transaction stack is used.
IDs and session tokens are validated and copied into caller-pool storage.

## Service context

An active trace has an optional case-sensitive key/value context for request-local service propagation. Obtain its
`MessageTrace` view from the root. The table is a fiber2 extension and is not encoded into CAT NT1/PT1 messages. HTTP
or gRPC integration may populate it from approved inbound metadata and synchronously copy it into an outbound request:

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
arguments borrow caller-pool storage; callers must copy them before retaining them asynchronously or resetting the
pool. Context access is owner-EventLoop-local. It becomes `RecordError::Completed` after final message completion.

`RecordLimits` independently bounds context entry count, key size, value size, and cumulative arena bytes. Removed or
replaced storage is not subtracted from that trace's byte budget because arena allocations are not reclaimed
individually. Applications should apply an outbound propagation allowlist instead of forwarding arbitrary context to
untrusted destinations.

## Transactions and events

Create a trace root and all its child messages on one running EventLoop. Handles may live across coroutine suspension.
Parent-child relationships are explicit, so interleaved coroutines on one loop do not share an implicit transaction
stack. There are exactly two public root factories, both on `CatClient`; roots cannot be created from a
`MessageTrace` view.

```cpp
fiber::mem::BufPool pool;
auto root_result = client->create_isolated_transaction(pool, "URL", "/orders");
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
`set_type()` and `set_name()` allow a Transaction or Event to bind routing information discovered after creation.
They are owner-EventLoop-local and affect the final encoded and aggregated key only while the handle remains valid.
Repeated `add_data()` calls use CAT's official `&` separator by default. A Transaction may call
`set_data_separator(' ')` before its first data entry when an application intentionally wants space-separated opaque
Log View data; only `&` and space are accepted, and changing the separator after data has been added is rejected.

A parent may complete before children that were already created. Its internal data remains in the trace arena until
the final open child completes, but the consumed parent handle cannot add more children. The trace remains valid until
that final child completes; afterwards CAT destroys its non-trivial state without resetting the caller's pool. Views
into the internal message tree are intentionally not part of the public API.

Type, name, status, message count, child count, per-message data, and total tree memory are bounded by `RecordLimits`.
Message strings and rendered data are copied into the supplied pooled storage at record time. Transactions store child
pointers in linked fixed-capacity chunks of 16; message data is rendered into linked byte chunks. Replacing a type or
name does not reclaim its previous arena bytes, so repeated changes continue to consume the bounded tree budget.

## Encoding, sampling, and aggregation

The private NT1 encoder follows the official CAT C client field order and framing: a four-byte big-endian payload
length, the `NT1` header, and the depth-first message body. PT1 uses the same frame prefix and the official text header
plus `t/T/A/E/M/H` line forms. Both encoders perform a counting pass followed by one exact `IoBuf` allocation.

An internally core-bound trace synchronously encodes when its final open message completes. The core receives an
independently owned buffer, after which CAT destroys `MessageTraceData`; arena memory remains owned by the caller's
pool. Encoding failures are reported to the core without submitting a partial frame.

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

On Linux, `enable_system_stats` adds the CAT 3.0-compatible `system.process` extension. `/proc/loadavg`, `/proc/stat`, and
`/proc/meminfo` provide host load, CPU, memory, swap, runnable-process, interrupt, and context-switch fields using the
official `load.*`, `cpu.*`, and `mem.*` names. `/proc/self/stat` and `/proc/self/statm` additionally provide
`process.cpu.*`, `process.rss.bytes`, and `process.virtual.bytes` for this client process. Host CPU percentages and
process CPU share require two valid samples, so the first heartbeat intentionally omits them. Host memory values describe
the machine visible through procfs, not a cgroup memory limit.

System collection is best-effort. Missing or malformed procfs input omits only unavailable fields, increments
`heartbeat_provider_failures`, and still sends the base heartbeat. Optional system fields are rolled back as one complete
XML extension if the configured field or byte budget cannot hold them; they never make an otherwise valid heartbeat fail.
All `extensionDetail` values are numeric as required by the CAT 3.0 status schema. String identity and version information
remain in the message-tree header and startup `System/Reboot` events instead of the heartbeat XML.

`CatClientStats` separates record truncation, sampling-to-aggregate conversion, aggregate retry/drop, encode failures,
normal/system admission, Router changes, connect failures, `WouldBlock`, partial-frame loss, Metric activity, and
heartbeat submission/delivery. Values are atomically readable from any thread.

## Layout

- `include/fiber/cat/`: public client/configuration API, single-pointer recording handles, and value types.
- `src/`: private trace/message layout, NT1/PT1 encoders, bounded aggregation, Router parser, sender, connection manager,
  message-ID generator, Linux system-statistics collector, heartbeat builder, and metric implementations.
- `tests/`: lifecycle, limits, chunk boundaries, routing, cross-loop collector transport, metrics, and coroutine
  interleaving tests.
