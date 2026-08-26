# Prometheus Metrics

## Overview

`fiber::prometheus` is a static metrics library for fixed-schema, high-performance services. It records Counter,
integer Gauge, and Histogram values in EventLoop-owned shards and exports Prometheus text exposition format 0.0.4.

The record path uses pre-bound handles and plain integer updates. It does not allocate, lock, perform atomic RMW, or
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
ctest --test-dir build -R '^(MetricValueTest|MetricsRegistryTest|TextEncoderTest|MultiLoopSnapshotTest)\.'
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

Metric handles are non-owning. The Registry and its shard storage must outlive every handle, and each handle may only
be used on its shard's owner EventLoop. Integer overflow is outside the v1 contract.

## Collection and shutdown

Collection must be awaited from a running EventLoop. `IoBufNodePool` and output objects belong to that collector loop;
snapshot callbacks only touch their own snapshot slot. Overlapping collections share one stable shard snapshot
generation, then encode independently into their caller-owned output. Calls with different `CollectOptions` therefore
share metric values without sharing output storage or output errors.

```cpp
auto result = co_await registry.collect_text(
        fiber::event::EventLoop::current().io_buf_node_pool(),
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
3. Stop and join every shard owner EventLoop.
4. Destroy workers and their handles, then destroy the Registry.

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
final values so the update loops remain observable.

## Layout

- `include/fiber/prometheus/`: public schema, Registry, shard, and metric-handle API.
- `src/`: Registry/storage, snapshot orchestration, and text encoding.
- `tests/`: value, schema, encoding, concurrency, and lifecycle tests.
- `bench/`: opt-in record-path benchmark.
