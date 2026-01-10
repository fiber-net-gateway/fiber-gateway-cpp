# TcpListener Design (Single-Coroutine Accept, Linux)

## Goals
- Provide coroutine-based TCP accept on a single `EventLoop` thread.
- Enforce a single owning coroutine for `accept()` calls (lifetime binding).
- Avoid reentrancy hazards by keeping accept logic inside the loop thread.
- Use non-blocking sockets with `epoll` readiness.
- No waiter queue: at most one active wait at any time.

## Constraints
- `accept()` must be called on the associated `EventLoop` thread.
- Only the first coroutine that calls `accept()` becomes the owner.
  - Any other coroutine attempting `accept()` returns `EBUSY`.
- Waiting coroutine always resumes on the same loop thread.
- No multi-thread accept or cross-thread close behavior.

## API Sketch
```cpp
namespace fiber::net {

struct ListenOptions {
    int backlog = SOMAXCONN;
    bool reuse_addr = true;
    bool reuse_port = false;
    bool v6_only = false;
};

struct AcceptResult {
    int fd = -1;
    SocketAddress peer{};
};

class TcpListener : public common::NonCopyable, public common::NonMovable {
public:
    class AcceptAwaiter;

    explicit TcpListener(event::EventLoop &loop);
    ~TcpListener();

    common::IoResult<void> bind(const SocketAddress &addr,
                                const ListenOptions &options);
    bool valid() const noexcept;
    int fd() const noexcept;
    void close();

    AcceptAwaiter accept() noexcept;
};

} // namespace fiber::net
```

## State Model
- `Closed`: `fd < 0`. No accepts possible.
- `Idle`: valid fd, no active waiter.
- `Waiting`: one active waiter, poller watching `EPOLLIN`.

## Accept Flow
1. `accept()` creates `AcceptAwaiter`.
2. `await_suspend(handle)`:
   - Verifies loop-thread and owner coroutine identity.
   - Tries `accept4` immediately.
   - On success or fatal error: store result/IoErr and return `false` (no suspend).
   - On `EAGAIN`: arm waiter and register `EPOLLIN`.
3. `epoll` callback:
   - Attempts `accept4`.
   - On success or fatal error: fills result, clears waiter, stops watching.
   - Directly `resume()` the coroutine (no `post`).
   - After `resume()`, callback must return without touching listener state.

## Resume Mechanics (Direct Resume)
- Resume happens inline in the IO callback to avoid extra scheduling hops.
- All listener state changes happen before `resume()`:
  - detach waiter
  - stop watching
  - fill result
- Once `resume()` is called, the callback must not access `this` or the waiter.

## Cancellation
- `AcceptAwaiter` destructor cancels the wait if still pending.
- Cancellation removes the waiter and disables `EPOLLIN`.
- No resume is performed on cancellation.

## Error Semantics
- `accept()` returns `IoResult<AcceptResult>`:
  - `WouldBlock` => keep waiting.
  - `Interrupted` => retry.
  - `ConnAborted` => ignored and retried.
  - Other errors => delivered as `unexpected(IoErr)`.
- Owner mismatch => `Busy`.
