# CAT Client Library

## Overview

`apps/cat` is the reusable native CAT client library for applications under `apps/`. Consumers link the `fiber::cat`
target and include headers from `fiber/cat/`.

The current implementation provides the owner-EventLoop-local recording layer and the private NT1 encoding boundary.
It supports explicit Transaction trees, Event leaves, and pre-bound Count/Duration Metric aggregators. Router
discovery, automatic CAT message IDs, PT1 encoding, cross-thread submission, collector transport, sampling, heartbeat,
and the public client lifecycle are intentionally not part of this stage.

## Build and test

```bash
cmake -S . -B build -DFIBER_BUILD_APPS=ON -DFIBER_BUILD_TESTS=ON
cmake --build build --target fiber_cat_tests
ctest --test-dir build -R '^(CatMessageTest|CatMetricTest)\.'
```

## Transactions and events

Create roots and all child messages on a running EventLoop. Handles are move-only, contain one private data pointer,
and may live across coroutine suspension. Parent-child relationships are explicit, so interleaved coroutines on one
loop do not share an implicit transaction stack.

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
reported to the core without submitting a partial frame. Public root factories are not yet attached to a client core,
so they retain their recording-only behavior until the public client lifecycle is added.

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

- `include/fiber/cat/`: public single-pointer recording handles and value types.
- `src/`: private trace, message, fixed child chunk, flat data chunk, and metric implementations.
- `tests/`: handle lifecycle, limits, chunk boundaries, metrics, and coroutine interleaving tests.
