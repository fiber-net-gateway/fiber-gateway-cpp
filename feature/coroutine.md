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

## Destruction-Cancels Contract

Cancellation in this library is frame destruction, never a generic in-flight
`cancel()` on a `Task`. A running `Task` is an opaque coroutine tree: the outer
holder has only a `coroutine_handle`, C++ coroutines offer no reflection to the
innermost suspended awaiter, and threading cooperative cancellation tokens
through every protocol layer would duplicate what `IoErr`/abort already
express. So the cancellation paths are:

- `TaskSelectAwaiter::~TaskSelectAwaiter()` runs `handle_.destroy()` -- destroying
  the whole frame tree, which runs every in-flight awaiter's destructor;
- `when_any`'s `destroy_losers()` and `timeout_for`'s temporary destruction are
  that same mechanism seen from the combinators.

Any `SelectableAwaiter` destroyed while suspended must:

1. de-register everything it registered on its owning loop -- timers, poller
   subscriptions, waiter queues, kernel state;
2. never resume the awaiting coroutine from the destructor;
3. make an already-queued resume retractable before the awaiter storage goes
   away (`WaitAwaiter`'s cancellable local defer queue is the reference
   pattern; an MPSC entry cannot be retracted);
4. be nothrow-destructible (enforced by the `SelectableAwaiter` concept).

Rules 1-3 cannot be checked at compile time; they are pinned by the
destruction-safety regressions (`AwaiterDestructionTest` for the fd family and
`timeout_for`-over-`Task::select()`, `WhenAnyTest` for the combinator losers,
`QuicLocalStreamGateTest` for the retractable-resume family).

### CancellableAwaiter (external active cancellation only)

`CancellableAwaiter` (`Awaitable.h`) is the leaf-only escape hatch: a party
other than the awaiting coroutine calls `cancel()` while the awaiter is
suspended. Its contract is deliberately strict: `cancel()` MUST resume the
awaiting coroutine exactly once, with a terminal error -- "may resume" would
leave callers unable to write correct code. `ConnectAwaiter` is the production
example (`DnsClient`'s inflight-cancel path). `timeout_for` / `when_any` must
never call it before their own resume (for a resuming `cancel()` that is a
double resume); they cancel through destruction. Note that
`CancellableAwaiter` does not imply `SelectableAwaiter`: `ConnectAwaiter`
satisfies the former and lacks `completed()` for the latter, while
`Watch::NextAwaiter`'s queue retraction is a destructor-only private helper
that does not resume and therefore must stay out of the concept.

### Task await is rvalue-only

`Task<T>::operator co_await()` is `&&`-qualified (like `Task::select()`): the
awaiter borrows the handle while the Task owns the frame, so only one await
chain may consume it. This turns three latent runtime bugs into compile errors
-- re-awaiting an lvalue (silently reads a moved-from result), awaiting a
moved-from Task (null-handle dereference), and two concurrent awaits of one
lvalue (the second `set_continuation` orphans the first parent). Code that
deliberately keeps ownership and drives the awaiter manually borrows with an
explicit `std::move(task).operator co_await()`.

