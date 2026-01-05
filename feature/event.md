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

    using Compare = bool (*)(const Node *a, const Node *b);

    void init();
    Node *min() const;
    std::size_t size() const;
    bool empty() const;

    void insert(Node *node, Compare less_than);
    void remove(Node *node, Compare less_than);
    void dequeue(Compare less_than);
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

    TimerHandle post_after(std::chrono::steady_clock::duration delay, TaskFn fn);
    TimerHandle post_at(std::chrono::steady_clock::time_point when, TaskFn fn);
    void cancel(TimerHandle handle);

    WatchHandle watch_fd(int fd, IoEvent events, IoCallback cb, WatchReady on_ready = {});
    void update_fd(WatchHandle handle, IoEvent events);
    void unwatch_fd(WatchHandle handle);
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
- `src/event/EpollPoller.h|.cpp`
- `src/event/TimerQueue.h|.cpp` (libuv heap translation)
- `src/event/MpscQueue.h` (header-only)
- `src/async/Scheduler.h`
- `src/async/Coroutine.h|.cpp`

## Implementation Notes
- `watch_fd` returns immediately. Actual registration occurs asynchronously on the loop thread.
- All loop-side execution is strictly async (no inline execution in `post`).
- The poller and timer queue are loop-thread only.
