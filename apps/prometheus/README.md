# Prometheus Metrics

## Overview

`fiber::prometheus` is a static metrics library for fixed-schema, high-performance services. It records static Counter,
integer Gauge, and Histogram values in EventLoop-owned shards, and dynamic Counter/Histogram labels in bounded,
shared LRU stores. It exports Prometheus text exposition format 0.0.4.

The static record path uses pre-bound handles and plain integer updates. It does not allocate, lock, perform atomic RMW, or
look up strings. Collection is a separate slow path: the Registry asks every owner EventLoop to copy its shard,
waits for a synchronized snapshot, aggregates the fixed series, and writes the result directly to an `IoBufChain` or
caller-provided `IoBuf`.

The module does not provide an HTTP server or `/metrics` route. An HTTP layer can use the returned chain as its
response body and remains responsible for Content-Type and body-completion semantics.

## Build

Select the component independently of runnable applications:

```bash
cmake -S . -B build -DFIBER_BUILD_APPS=OFF -DFIBER_BUILD_PROMETHEUS=ON
cmake --build build --target fiber_prometheus_tests
ctest --test-dir build -R '^(MetricValueTest|MetricsRegistryTest|TextEncoderTest|MultiLoopSnapshotTest|DynamicSeriesTest|DynamicSnapshotTest)\.'
```

Link consumers against `fiber::prometheus`.

## Setup and recording

All families, fixed label series, and worker shards must be registered before `freeze()`. IDs carry their Registry
identity, so a family or series from another Registry is rejected during setup.

```cpp
using namespace fiber::prometheus;

MetricsRegistry registry;
constexpr std::array<std::string_view, 1> label_names{"method"};
auto requests_family = registry.register_counter(
        "http_requests_total", "Total HTTP requests", label_names);

constexpr std::array<std::string_view, 1> get_labels{"GET"};
auto requests_get = registry.register_series(*requests_family, get_labels);
auto worker_id = registry.add_shard(worker_loop);

auto frozen = registry.freeze();
auto requests = registry.shard(*worker_id)->counter(*requests_get);

// Only on worker_loop after successful setup:
requests->inc();
```

Counter names must end in `_total`. Histogram upper bounds are finite integer values and must be strictly increasing.
For duration histograms, select `Nanoseconds`, `Microseconds`, `Milliseconds`, or `Seconds`; bucket bounds and sums are
then rendered as exact seconds values. `Raw` keeps integer values unchanged.

Static metric handles are non-owning. The Registry and its shard storage must outlive every handle, and each handle may only
be used on its shard's owner EventLoop. Integer overflow is outside the v1 contract.

## Dynamic labels and strict family LRU

Family name, type, help, label names, buckets and capacity are fixed before `freeze()`. Label values may arrive at
runtime. Each LRU family retains at most `max_series` complete label tuples across all recording threads in this
Registry. Each family has its own mutex, fixed hash index and reusable slots. Counter and Histogram share one
aggregate per tuple; dynamic Gauge is not supported.

```cpp
constexpr std::array<std::string_view, 1> user_label_names{"user"};
auto users_family = registry.register_counter(
        "user_requests_total", "Requests by user", user_label_names,
        {.mode = SeriesMode::Lru, .max_series = 1024, .max_storage_bytes = 1024 * 1024});
// Register other families/shards, check setup results, then freeze.
auto frozen = registry.freeze();
// After successful freeze, bind a copyable handle and publish it to workers.
auto users = registry.dynamic_counter(*users_family);

// On any thread; no EventLoop is required for recording:
const std::array<std::string_view, 1> values{username};
auto recorded = users->inc(values);
if (!recorded) {
    // Apply the application's instrumentation failure policy using recorded.error().
}
```

`register_histogram()` accepts the same `SeriesOptions` as its final argument. Bind with
`dynamic_histogram(family_id)` and call `observe(values, integer_value)`. `DynamicCounter::add(values, delta)`
accepts zero: it creates a zero entry or refreshes an existing entry's recency. Labels are copied when created,
compared by complete string contents, and exported in deterministic unsigned-byte tuple order. Empty label values
and an empty label schema are allowed. A dynamic family cannot use `register_series()` or static series Refs.

Successful recording updates the value and moves the tuple to the MRU head under the same lock. At capacity, a new
tuple replaces exactly the least recently successfully recorded tuple. Its entire aggregate is discarded. Reappearing
labels start from zero, including every Histogram bucket, count and sum. Collection and diagnostics never touch
recency. Invalid labels, allocation failure or budget failure preserve existing values, tuples and LRU order.

Dynamic handles do not own the Registry and cannot be default-constructed. They may be copied across threads after
normal synchronized publication. Every runtime record call is `[[nodiscard]]`, `noexcept` and returns `IoResult<void>`:
`Invalid` for wrong label count or escaped length, `NoMem` for allocation failure, and `MessageTooLarge` for an
explicit memory budget failure. Binding rejects an unfrozen Registry, foreign family, wrong type or storage mode.
Stable hits allocate nothing. Creating/replacing a label allocates its owned length/byte block before committing the
replacement. Memory is reclaimed immediately on eviction; the hash table has no tombstones or historical arena.

`max_storage_bytes = 0` uses the finite bound implied by capacity, label count, escaped label limit and bucket count.
A nonzero budget includes fixed store/slot/value/index storage, allocator usable size and block-header allowance,
and the unpublished candidate alongside the old tuple. An insufficient fixed budget fails `freeze()`; a recording
budget failure never evicts extra entries to make space. Freeze allocation failure rolls back all runtime storage and
allows retry. `diagnostics()` returns a locked copy of active count, capacity, storage bytes, evictions and creation
failures without registering extra metrics.

`RegistryOptions::max_dynamic_snapshot_bytes` defaults to 64 MiB. Snapshot buffers reserve entry/view/value capacity
for N tuples and grow their owned label-byte tail as needed; capacities are retained across generations. The shared
budget includes all family buffers and both old/new allocations during growth. Initial reservation can fail freeze;
later capture failure fails the entire generation for every attached collector. Output limits remain per caller.
Snapshot copying holds one family mutex for a complete copy, then sorts and encodes after unlocking. Each generation
owns its labels and values, so live eviction cannot invalidate an in-progress scrape. No global instantaneous view
across different families and static shards is promised. Empty families omit HELP/TYPE.

Eviction resets cumulative metrics. A tuple created and evicted between scrapes can be missed entirely; `rate()` and
`increase()` cannot recover every lost update. Record a separate static total when exact process totals are needed.
Client capacity does not bound Prometheus's retained historical cardinality. Strict LRU serializes a hot family and
can block request EventLoops under contention; measure the dynamic path for the intended workload.

## Collection and shutdown

Collection must be awaited from a running EventLoop. `IoBufNodePool` and output objects belong to that collector loop;
snapshot callbacks only touch their own snapshot slot. Overlapping collections share one stable shard snapshot
generation (including owned dynamic copies), then encode independently into their caller-owned output. Calls with different `CollectOptions` therefore
share metric values without sharing output storage or output errors.

```cpp
auto result = co_await registry.collect_text(
        {.chunk_size = 4096, .max_output_bytes = 16 * 1024 * 1024});
```

`collect_text_into()` writes only into the current writable tail and commits once after complete success. On capacity
or output-limit failure, the existing readable region is unchanged. `collect_text()` builds a local chain and never
marks it complete.

The first collection in a generation copies the local shard and posts one snapshot request to every remote shard owner.
Later overlapping collections await those same requests, including calls that arrive after the snapshots are ready but
while another caller is still encoding. The Registry does not overwrite the snapshot slots until every attached caller
has finished and every posted callback has returned.

Shut down in this order:

1. Call `stop_collecting()` to reject new calls with `IoErr::Canceled`.
2. Await `wait_for_idle()` on a running EventLoop.
3. Stop and join all metric writers, worker threads and shard owner EventLoops.
4. Destroy workers and their handles, then destroy the Registry.

`stop_collecting()` does not stop dynamic recording, and `wait_for_idle()` waits for collection only.

Destroying a Task that is waiting for snapshots detaches only that caller and does not invalidate posted callbacks or
other callers sharing the generation. The Registry owns the request and snapshot state and remains active until all
callbacks and attached callers finish.

## Benchmark

A small repeatable record-path benchmark is available as an opt-in target:

```bash
cmake -S . -B build -DFIBER_BUILD_PROMETHEUS=ON -DFIBER_BUILD_PROMETHEUS_BENCHMARK=ON
cmake --build build --target prometheus_record_benchmark
./build/components/prometheus/prometheus_record_benchmark 100000000
```

Run it from a Release build when comparing changes. It reports Counter and Histogram operations per second and the
final values so the update loops remain observable. The same CMake option also builds:

```bash
./build/components/prometheus/prometheus_dynamic_benchmark 50000
```

This runs Counter+Histogram operation pairs: 1/2/4/8-thread steady hits on shared and separate families, cold creation,
full-capacity churn, concurrent scrape at a 1 ms target interval, and 4096-byte labels with 128 buckets. It reports
throughput, sampled p95/p99 pair latency (one sample per 64 pairs), recording label allocations derived from successful
creations, current store memory and maximum end-to-end scrape time. Churn throughput includes allocation-free label generation; sampled pair latency measures the two record calls.
The benchmark allows up to 64 MiB of output per scrape so the large-label case can export all buckets.
It does not directly instrument mutex hold time or count output/snapshot allocator calls. See
[the validation report](../../feature/prometheus_dynamic_series_lru_validation.md) for local measurements and limits.

## Layout

- `include/fiber/prometheus/`: public schema, Registry, shard, and metric-handle API.
- `src/`: Registry/storage, snapshot orchestration, and text encoding.
- `tests/`: value, schema, encoding, concurrency, and lifecycle tests.
- `bench/`: opt-in record-path benchmark.
