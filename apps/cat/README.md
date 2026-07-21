# CAT Client Library

## Overview

`apps/cat` is the reusable native CAT client library for applications under `apps/`. Consumers link the `fiber::cat`
target and include headers from `fiber/cat/`.

The current implementation is the owner-EventLoop-local recording layer. It provides explicit Transaction trees,
Event leaves, and pre-bound Count/Duration Metric aggregators. Router discovery, CAT message IDs, NT1/PT1 encoding,
cross-thread submission, collector transport, sampling, heartbeat, and client lifecycle are intentionally not part of
this stage.

## Build and test

```bash
cmake -S . -B build -DFIBER_BUILD_APPS=ON -DFIBER_BUILD_TESTS=ON
cmake --build build --target fiber_cat_tests
ctest --test-dir build -R '^(CatMessageTest|CatMetricTest)\.'
```

## Transactions and events

Create roots and all child messages on a running EventLoop. Handles are move-only, stay bound to that EventLoop, and
may live across coroutine suspension. Parent-child relationships are explicit, so interleaved coroutines on one loop
do not share an implicit transaction stack.

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

Status `"0"` is success; every other status marks the tree as a problem. Completion is idempotent. Destroying an
unfinished Transaction or Event completes it with `CAT_CLIENT_INCOMPLETE`, preventing an abandoned operation from
being reported as success. A root may complete before its children, but the tree becomes ready only after every open
message has completed.

Type, name, status, message count, child count, per-message data, and total tree memory are bounded by `RecordLimits`.
Message strings and data are copied into tree-owned pooled storage at record time.

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

- `include/fiber/cat/`: public recording API.
- `src/`: pooled message-tree and metric implementations.
- `tests/`: message semantics, limits, metrics, and coroutine interleaving tests.
