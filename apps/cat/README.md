# CAT Client Library

## Overview

`apps/cat` is the reusable native CAT client library for applications under `apps/`. Consumers link the `fiber::cat`
target and include headers from `fiber/cat/`.

The current implementation provides owner-EventLoop-local recording, NT1 encoding, a public client lifecycle, CAT
router discovery, bounded cross-thread submission, collector transport, and router-provided sampling/block policy. It
supports explicit Transaction trees, Event leaves, and pre-bound Count/Duration Metric aggregators. Automatic CAT
message-ID generation, PT1 encoding, metric transport, and heartbeat messages are not part of this stage.

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
        auto trace_result = fiber::cat::MessageTrace::create(**client, {}, {.message_id = "message-id"});
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

Call `co_await client->shutdown()` before stopping the sender EventLoop. Shutdown stops accepting new frames, waits
for submitters already inside the MPSC handoff, drains the connected sender up to `shutdown_drain_timeout`, drops the
remainder, closes the collector socket, and completes in `Stopped` state.

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

A parent may complete before children that were already created. Its internal data remains in the trace arena until
the final open child completes, but the consumed parent handle cannot add more children. The final completion destroys
the complete trace arena in one operation. Views into the internal tree are intentionally not part of the public API.

Type, name, status, message count, child count, per-message data, and total tree memory are bounded by `RecordLimits`.
Message strings and rendered data are copied into trace-owned pooled storage at record time. Transactions store child
pointers in linked fixed-capacity chunks of 16; message data is rendered into linked byte chunks.

## NT1 encoding

The private NT1 encoder follows the official CAT C client field order and framing: a four-byte big-endian payload
length, the `NT1` header, and the depth-first message body. It performs a counting pass followed by one exact `IoBuf`
allocation. Message data is copied directly from its chunks into that buffer.

An internally core-bound trace synchronously encodes when its final open message completes. The core receives an
independently owned buffer, after which the complete trace arena is immediately destroyed. Encoding failures are
reported to the core without submitting a partial frame.

## Router and collector transport

The control plane is coroutine-based. It periodically fetches
`/cat/s/router?op=json&domain=...&ip=...&hostname=...`, resolves router hostnames, parses bounded
`kvs.routers/sample/block` responses, rotates collectors, and reconnects with capped exponential backoff. A failed
refresh retains the last usable collector set.

The data plane does not use a sender coroutine. Finalized traces reserve a packed message/byte budget, enter an MPSC
queue, and post one coalesced notification to the sender loop. That callback batches up to 16 complete NT1 frames and
calls `try_writev`. `WouldBlock` arms a writable callback and a write deadline; fairness limits defer continued pumping
through a local callback. Frames are concatenated on the raw TCP stream. If a connection fails after writing only a
prefix of a frame, that partial frame is dropped rather than resumed on a new collector connection.

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

Metric snapshots are the boundary a later CAT aggregation/encoding layer will consume. A snapshot's name view remains
valid only while its Metric remains alive.

## Layout

- `include/fiber/cat/`: public client/configuration API, single-pointer recording handles, and value types.
- `src/`: private trace/message layout, NT1 encoder, router parser, sender, connection manager, and metric
  implementations.
- `tests/`: lifecycle, limits, chunk boundaries, routing, cross-loop collector transport, metrics, and coroutine
  interleaving tests.
