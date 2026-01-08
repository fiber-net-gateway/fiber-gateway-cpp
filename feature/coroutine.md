# Coroutine Frame Allocation (TLS Pool, EventLoop-bound)

## Goals
- Reduce coroutine frame allocation overhead by using a fast thread-local pool.
- Bind coroutine lifetime to an EventLoop thread to avoid cross-thread deallocation costs.
- Keep promise and Task code unchanged at call sites; allocation is transparent.

## Core Assumptions
- Each EventLoop runs on a dedicated thread.
- Coroutines created on an EventLoop thread are destroyed on the same thread.
- Cross-thread destruction is not allowed; debug checks should enforce this.

## Allocation Strategy
- Use a per-thread TLS pool with size classes (e.g., 64/128/256/512/1024/2048/4096 bytes).
- Each coroutine frame is preceded by a small header:
  - size class id
  - original allocation size (for large allocations and diagnostics)
- Allocation steps:
  1) Pick size class for `sizeof(frame) + header`.
  2) If class fits, pop from the free list; else allocate from upstream allocator.
  3) Write the header and return the frame pointer after the header.
- Deallocation steps:
  1) Read the header by subtracting header size from the frame pointer.
  2) If class id is "large", free via upstream allocator.
  3) Otherwise, push into the TLS pool free list.

## TLS Pool Binding
- EventLoop owns a `CoroutineFramePool`.
- On EventLoop thread start, set TLS pointer:
  - `CoroutineFramePool::set_current(&pool);`
- Optional RAII helper for tests or synchronous paths:
  - `CoroutineFrameAllocScope scope(&pool);`

## Promise Integration
- Introduce `CoroutinePromiseBase` with custom `operator new/delete`.
- `TaskPromiseBase` inherits from `CoroutinePromiseBase` so all tasks use the pool.
- `operator new` uses `CoroutineFramePool::current()` and falls back to upstream allocator when
  no TLS pool is set (e.g., tests without EventLoop).
- `operator delete` expects the same TLS pool as allocation; debug builds assert that the
  current pool matches the header's pool.

## Debug and Safety Checks
- In debug builds:
  - assert TLS pool is set when allocating in EventLoop code paths
  - assert `current_pool == header.pool` on delete
  - optional tracking counters per pool for leak checks on shutdown

## Integration Points
- EventLoop thread entry: install TLS pool before running loop.
- `TaskPromiseBase` inherits `CoroutinePromiseBase`.
- `Task` destruction path uses `handle.destroy()` which triggers promise delete.

## Limitations
- Coroutines must not outlive their EventLoop thread.
- No cross-thread destruction; callers must transfer work via `post()` instead.

