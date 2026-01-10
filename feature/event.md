# Event Loop Design (Linux)

## Goals
- Linux-only implementation based on `epoll` + `eventfd`.
- Multi-producer, single-consumer task queue (MPSC) for cross-thread scheduling.
- Strictly asynchronous execution on the loop thread (no inline execution during `post`).
- Timers integrated into a single event loop.
- Defer entries use fixed callbacks with optional cancel handling.

## Threading Model
- One event loop thread owns the poller and internal state.
- Any thread may call `post(DeferEntry&)`, `cancel(DeferEntry&)`, `post_at`, `cancel`, or `stop`.
- Cross-thread calls enqueue into the defer MPSC and signal `eventfd` to wake the loop.

## Loop Group
`EventLoopGroup` owns a fixed set of loops and a `ThreadGroup`. `start()` runs one
loop per thread, and `post()` schedules to the current loop when called from a
loop thread (otherwise it uses round-robin selection).

## Wakeup Strategy
- `eventfd(EFD_NONBLOCK | EFD_CLOEXEC)` is registered in `epoll`.
- Producers write `uint64_t(1)` to `eventfd` to wake the loop.
- The loop drains `eventfd` before processing queued commands.
- An atomic flag (`wakeup_pending_`) prevents redundant wakeups.

## Defer Queue
`DeferEntry` provides an intrusive, fixed-callback scheduling primitive:
- `post(DeferEntry&)` enqueues the entry for execution on the loop thread.
- `cancel(DeferEntry&)` marks the entry canceled; the loop executes `on_cancel` when drained.
- Each entry has two callbacks: `on_run` and `on_cancel` (either may be null).
- Cancellation is best-effort if it races with draining.

## TimerQueue (Heap)
`TimerQueue` is a C++ translation of `libuv`'s `heap-inl.h`, used as an intrusive
min-heap for timer nodes. It provides heap primitives and does not own timer data.

```cpp
class TimerQueue {
public:
    struct Node {
        Node *left;
        Node *right;
        Node *parent;
    };

    void init();
    Node *min() const;
    std::size_t size() const;
    bool empty() const;

    void insert(Node *node);
    void remove(Node *node);
    void dequeue();
};
```

## API Summary
```cpp
namespace fiber::event {

class EventLoop : public fiber::async::IScheduler {
public:
    void run();
    void run_once();
    void stop();

    void post(DeferEntry &entry);
    void cancel(DeferEntry &entry);

    void post_at(std::chrono::steady_clock::time_point when, TimerEntry &entry);
    void cancel(TimerEntry &entry);
};

class EventLoopGroup : public fiber::async::IScheduler {
public:
    explicit EventLoopGroup(std::size_t size);

    void start();
    void stop();
    void join();

    EventLoop &at(std::size_t index);

};

} // namespace fiber::event
```

## Loop Cycle
1) Drain defer queue and execute due callbacks.
2) Execute due timers.
3) `epoll_wait` with timeout from the next deadline.
4) Dispatch IO callbacks.

## File Layout
- `src/event/EventLoop.h|.cpp`
- `src/event/Poller.h|.cpp`
- `src/event/TimerQueue.h|.cpp` (libuv heap translation)
- `src/event/MpscQueue.h` (header-only)
- `src/async/Scheduler.h`
- `src/async/Coroutine.h|.cpp`

## Implementation Notes
- All loop-side execution is strictly async (no inline execution in `post`).
- The poller and timer queue are loop-thread only.
