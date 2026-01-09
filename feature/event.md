# Event Loop Design (Linux)

## Goals
- Linux-only implementation based on `epoll` + `eventfd`.
- Multi-producer, single-consumer task queue (MPSC) for cross-thread scheduling.
- Strictly asynchronous execution on the loop thread (no inline execution during `post`).
- Timers and FD watching integrated into a single event loop.

## Threading Model
- One event loop thread owns the poller and internal state.
- Any thread may call `post`, `watch_fd`, `update_fd`, `unwatch_fd`, `cancel`, or `stop`.
- Cross-thread calls enqueue a command into MPSC and signal `eventfd` to wake the loop.

## Loop Group
`EventLoopGroup` owns a fixed set of loops and a `ThreadGroup`. `start()` runs one
loop per thread, and `post()` schedules to the current loop when called from a
loop thread (otherwise it uses round-robin selection).

## Wakeup Strategy
- `eventfd(EFD_NONBLOCK | EFD_CLOEXEC)` is registered in `epoll`.
- Producers write `uint64_t(1)` to `eventfd` to wake the loop.
- The loop drains `eventfd` before processing queued commands.
- An atomic flag (`wakeup_pending_`) prevents redundant wakeups.

## Command Queue
Commands are enqueued to the MPSC queue and consumed on the loop thread:
- `Task`: run a user-provided function.
- `WatchAdd`: add a file descriptor to `epoll`.
- `WatchMod`: modify interest mask.
- `WatchDel`: remove a file descriptor.
- `TimerAdd` / `TimerCancel`: update the timer heap.
- `Stop`: break the loop.

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
    using TaskFn = std::function<void()>;
    using IoCallback = std::function<void(IoEvent)>;
    using WatchReady = std::function<void(int error)>;

    void run();
    void run_once();
    void stop();

    void post(TaskFn fn);
    void post(std::coroutine_handle<> handle) override;

    void post_at(std::chrono::steady_clock::time_point when, TimerEntry &entry);
    void cancel(TimerEntry &entry);

    WatchHandle watch_fd(int fd, IoEvent events, IoCallback cb, WatchReady on_ready = {});
    void update_fd(WatchHandle handle, IoEvent events);
    void unwatch_fd(WatchHandle handle);
};

class EventLoopGroup : public fiber::async::IScheduler {
public:
    explicit EventLoopGroup(std::size_t size);

    void start();
    void stop();
    void join();

    EventLoop &at(std::size_t index);

    void post(TaskFn fn);
    void post(std::coroutine_handle<> handle) override;
};

} // namespace fiber::event
```

## Loop Cycle
1) Drain command queue and apply changes (task run, watch registration, timer updates).
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
- `watch_fd` returns immediately. Actual registration occurs asynchronously on the loop thread.
- All loop-side execution is strictly async (no inline execution in `post`).
- The poller and timer queue are loop-thread only.
