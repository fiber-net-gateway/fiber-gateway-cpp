# Signal Await Design (POSIX)

## Goals
- Provide coroutine-friendly signal waiting on Unix-like systems.
- Use portable POSIX primitives (`pthread_sigmask`, `sigwaitinfo`).
- Resume waiting coroutines on the owning `EventLoop` thread.
- Best-effort FIFO fairness per signal.
- Cancellation-safe: waiter destruction cancels pending waits.

## Non-Goals
- No Linux-only `signalfd` or BSD-only `kqueue` in the baseline design.
- No strict fairness guarantees across signals.
- No timed wait yet (future generic timeout semantics can be layered).

## Threading Model
- All handled signals are **blocked** in all threads via `pthread_sigmask`.
- A dedicated `SignalDispatcher` thread calls `sigwaitinfo` and forwards
  deliveries into the target `EventLoop` (via defer queue).
- All waiter queues and pending queues are owned and mutated by the loop thread.

## Core Components

### SignalService (event layer)
Owns dispatcher thread, pending queues, and waiter queues. Binds to a single
`EventLoop` instance.

Responsibilities:
- Install signal mask (outside or at attach).
- Spawn dispatcher thread.
- Receive deliveries on loop thread and notify waiters.
- Manage cancellation and shutdown.

### SignalAwaiter (async layer)
Coroutine awaiter for a single signal.
- Checks `pending` first.
- Enqueues a waiter and suspends.
- Resumes on loop thread with `SignalInfo`.

## Data Structures
- `WaiterQueue[NSIG]`: FIFO for coroutines waiting on each signal.
- `PendingQueue[NSIG]`: FIFO of `SignalInfo` for signals received before a waiter.
- `Waiter` state machine: `Waiting -> Notified -> Resumed`, or `Waiting -> Canceled`.

## Delivery Flow
1) Dispatcher thread waits for signals using `sigwaitinfo(mask, &info)`.
2) For each signal, it posts a delivery item into the loop defer queue.
3) Loop thread dispatches:
   - If there is a waiter: pop FIFO waiter, mark `Notified`, store info,
     then post waiter resume via defer.
   - Else: push `SignalInfo` into `pending`.

## Cancellation Races
- `SignalAwaiter` destructor cancels if still `Waiting`.
- If `Notified`, mark `Canceled` so resume callback becomes a no-op.
- All queue operations happen on the loop thread; no external locks needed.

## Interface Skeleton (Aligned with Current Layout)
```cpp
// src/async/Signal.h
namespace fiber::async {

struct SignalInfo {
    int signum{};
    int code{};
    pid_t pid{};
    uid_t uid{};
    int status{};
    int errno_{};
    std::intptr_t value{};
};

class SignalSet {
public:
    SignalSet();
    SignalSet &add(int signum);
    SignalSet &remove(int signum);
    bool contains(int signum) const noexcept;
    const sigset_t &native() const noexcept;
private:
    sigset_t set_{};
};

class SignalAwaiter {
public:
    explicit SignalAwaiter(int signum);
    bool await_ready() noexcept;
    bool await_suspend(std::coroutine_handle<> handle);
    SignalInfo await_resume() noexcept;
private:
    int signum_{};
    // waiter is heap-allocated while suspended
};

SignalAwaiter wait_signal(int signum);

} // namespace fiber::async
```

```cpp
// src/event/SignalService.h
namespace fiber::event {

class SignalService {
public:
    explicit SignalService(EventLoop &loop);
    ~SignalService();

    // Attach installs the dispatcher and must run on the loop thread.
    bool attach(const fiber::async::SignalSet &mask);
    void detach();

    // Loop-thread only.
    void enqueue_waiter(int signum, void *waiter);
    bool try_pop_pending(int signum, fiber::async::SignalInfo &out);

private:
    struct Delivery {
        EventLoop::DeferEntry entry{};
        SignalService *service = nullptr;
        fiber::async::SignalInfo info{};
        static void on_run(Delivery *self);
        static void on_cancel(Delivery *self);
    };

    void on_delivery(const fiber::async::SignalInfo &info);
    void run_dispatcher();

    EventLoop &loop_;
    fiber::async::SignalSet mask_{};
    std::jthread dispatcher_{};

    // loop-thread only state:
    // WaiterQueue waiters_[NSIG];
    // PendingQueue pending_[NSIG];
};

} // namespace fiber::event
```

### Waiter Shape (Matches EventLoop::DeferEntry Pattern)
```cpp
// src/async/Signal.h (internal detail)
struct SignalWaiter {
    fiber::event::EventLoop::DeferEntry defer{};
    std::coroutine_handle<> handle{};
    fiber::event::EventLoop *loop = nullptr;
    std::atomic<State> state{State::Waiting};
    SignalInfo info{};
    SignalWaiter *prev = nullptr;
    SignalWaiter *next = nullptr;

    static void on_run(SignalWaiter *self);
    static void on_cancel(SignalWaiter *self);
};
```

The loop thread resumes a waiter by:
```
loop->post<SignalWaiter, &SignalWaiter::defer,
           &SignalWaiter::on_run, &SignalWaiter::on_cancel>(*waiter);
```

## Suggested File Layout
- `src/async/Signal.h|.cpp` (awaiter + public API)
- `src/event/SignalService.h|.cpp` (dispatcher + queues)
- `feature/signal.md` (this doc)

## Test Notes (GoogleTest + CTest)
Suggested tests live under `tests/SignalTest.cpp` and are wired to
`fiber_tests` in `CMakeLists.txt`.

1) **Single waiter receives signal**
   - Block `SIGUSR1` in the test thread.
   - Create `EventLoop`, `SignalService`, `attach` with mask {SIGUSR1}.
   - `spawn(loop, [&] { auto info = co_await wait_signal(SIGUSR1); ... });`
   - Raise `SIGUSR1` via `kill(getpid(), SIGUSR1)`.
   - Run loop until waiter resumes; assert `info.signum == SIGUSR1`.

2) **Pending delivery before await**
   - Send `SIGUSR1` before starting the awaiter.
   - `co_await wait_signal(SIGUSR1)` should be ready immediately.

3) **FIFO fairness for multiple waiters**
   - Queue two awaiters for `SIGUSR1`.
   - Send two signals; assert resume order matches enqueue order.

4) **Cancellation**
   - Start awaiter and destroy it before signal arrives.
   - Send signal, ensure it goes to next waiter or pending queue.

Notes:
- All signal tests must block the handled signals in the test thread to prevent
  default signal delivery.
- Avoid `SIGALRM`/`SIGCHLD` to reduce interference with the test runner.

## Signal Masking Strategy
- `EventLoopGroup::start` should call `pthread_sigmask(SIG_BLOCK, &mask, nullptr)`
  on each worker thread before `EventLoop::run()`.
- The dispatcher thread uses the same mask and waits via `sigwaitinfo`.

## Shutdown Semantics
- `detach()` requests dispatcher stop; call it when no active waiters remain.
- Pending signals are dropped on shutdown.

## Extensions (Future)
- Support "wait any" (multi-signal wait) using a shared token to avoid
  double-resume across multiple queues.
- Add Linux `signalfd` backend as an optional optimization with the same API.
