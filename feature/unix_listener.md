# Unix Domain Listener Design

Goal: add unix-domain socket listening that reuses the existing accept/wait
mechanics from TCP, while keeping protocol-specific address handling in a
separate type.

## Shared accept core (AcceptFd)

Introduce a small internal helper that owns the accept-wait state:

- `EventLoop &loop`
- `Poller::Item item`
- `int fd`
- `bool watching`
- `AcceptAwaiter *waiter`
- `void *owner`

Responsibilities:
- `begin_wait` enforces single owner + single waiter.
- `accept_once` is provided by a per-protocol hook (trait or vtable).
- `handle_acceptable` resumes a single waiter when fd becomes readable.
- `watch_read` / `unwatch_read` manage the poller registration.
- `close` cancels a waiter with `IoErr::Canceled` before closing the fd.

Resume policy stays the same as TCP today: resume directly after state changes
are complete (no deferred post).

## Address types

Keep IP addresses separate; add a unix-domain address type.

### UnixAddress

- Represents filesystem path and abstract namespace.
- Parsed/serialized via `sockaddr_un`.
- Supports unnamed addresses (for peers that do not provide a path).

## Listener types

### TcpListener

- Becomes a thin wrapper around `AcceptFd<TcpTraits>`.
- Still uses `SocketAddress` and `ListenOptions`.

### UnixListener

- New listener wrapping `AcceptFd<UnixTraits>`.
- Uses `UnixAddress` and `UnixListenOptions`.
- Optional `unlink_existing` handling for filesystem paths.

## Minimal interface skeleton

```cpp
// src/net/UnixAddress.h
namespace fiber::net {

enum class UnixAddressKind {
    Filesystem,
    Abstract,
    Unnamed,
};

class UnixAddress {
public:
    static UnixAddress filesystem(std::string path);
    static UnixAddress abstract(std::string bytes);
    static UnixAddress unnamed();

    UnixAddressKind kind() const noexcept;
    const std::string &path() const noexcept;
    const std::string &bytes() const noexcept;

    bool to_sockaddr(sockaddr_storage &out, socklen_t &len) const noexcept;
    static bool from_sockaddr(const sockaddr *addr, socklen_t len, UnixAddress &out) noexcept;
    std::string to_string() const;
};

} // namespace fiber::net
```

```cpp
// src/net/UnixListener.h
namespace fiber::net {

struct UnixListenOptions {
    int backlog = SOMAXCONN;
    bool unlink_existing = false;
};

struct UnixAcceptResult {
    int fd = -1;
    UnixAddress peer{};
};

class UnixListener : public common::NonCopyable, public common::NonMovable {
public:
    class AcceptAwaiter;

    explicit UnixListener(fiber::event::EventLoop &loop);
    ~UnixListener();

    fiber::common::IoResult<void> bind(const UnixAddress &addr,
                                       const UnixListenOptions &options);
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    void close();

    [[nodiscard]] AcceptAwaiter accept() noexcept;
};

} // namespace fiber::net
```

```cpp
// src/net/detail/AcceptFd.h (internal)
namespace fiber::net::detail {

template <typename Traits, typename AcceptResult>
class AcceptFd {
public:
    explicit AcceptFd(fiber::event::EventLoop &loop);
    ~AcceptFd();

    fiber::common::IoResult<void> bind(typename Traits::Address address,
                                       const typename Traits::ListenOptions &options);
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    void close();

    class AcceptAwaiter;
    [[nodiscard]] AcceptAwaiter accept() noexcept;
};

} // namespace fiber::net::detail
```

Notes:
- `Traits` supplies `accept_once(int fd, AcceptResult &out) -> IoErr` and
  `bind(...) -> IoResult<int>` (returns the new listener fd).
- `AcceptAwaiter` remains a nested type, identical semantics to `TcpListener`.

## Error semantics

- `bind` returns `IoErr::Already` when already bound.
- `accept` returns `IoErr::BadFd` if invalid, `IoErr::Busy` for non-owner or
  concurrent waiter.
- `close` cancels a waiting accept with `IoErr::Canceled`.
- `accept_once` maps `EAGAIN`/`EWOULDBLOCK` to `IoErr::WouldBlock`.

## Test notes

Create `tests/UnixListenerTest.cpp` with:
- `AcceptsConnection`: bind to filesystem path, connect, accept, verify peer.
- `PendingBeforeAwait`: connect before await, accept should return immediately.
- `OwnerMismatchReturnsBusy`: two coroutines awaiting accept; second gets Busy.
- `Cancellation`: destroy awaiter or call `close`, expect `IoErr::Canceled`.
- `UnlinkExisting`: when `unlink_existing` and path exists, bind succeeds.
- `AbstractAddress` (Linux): bind/connect via abstract namespace, accept ok.

Follow existing patterns in `tests/TcpListenerTest.cpp`.
