#ifndef FIBER_NET_DETAIL_STREAM_FD_H
#define FIBER_NET_DETAIL_STREAM_FD_H

#include <chrono>
#include <cstddef>
#include <sys/uio.h>

#include "../../async/Task.h"
#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../../event/EventLoop.h"
#include "RWFd.h"

namespace fiber::net::detail {

/**
 * Stream syscall adapter: owns EOF, peer write-side hangup, fatal errors and
 * the terminal callback. RWFd below it only tracks direction readiness.
 *
 * forbidden read/read and write/write in multi-coroutine, but read and write can overlap.
 * no internal read/write lock is enforced for performance.
 */
class StreamFd : public common::NonCopyable, public common::NonMovable {
public:
    using IoTask = fiber::async::Task<fiber::common::IoResult<size_t>>;
    using ReadyCallback = RWFd::ReadyCallback;
    using WaitReadableAwaiter = RWFd::WaitReadableAwaiter;
    using WaitWritableAwaiter = RWFd::WaitWritableAwaiter;

    StreamFd(fiber::event::EventLoop &owner_loop, int fd);
    ~StreamFd();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] fiber::event::EventLoop &owner_loop() const noexcept;
    [[nodiscard]] fiber::event::EventLoop &current_loop() const noexcept;
    [[nodiscard]] fiber::event::EventLoop &loop() const noexcept;
    [[nodiscard]] RWFd &rwfd() noexcept;
    [[nodiscard]] bool read_ready() const noexcept { return rwfd_.read_state() == RWFd::State::Ready; }
    [[nodiscard]] bool write_ready() const noexcept { return rwfd_.write_state() == RWFd::State::Ready; }
    // EOF observed on the read side; reads keep returning 0, writes still work.
    [[nodiscard]] bool eof() const noexcept { return eof_; }
    // Peer closed its write side; buffered data still reads out first.
    [[nodiscard]] bool peer_hangup() const noexcept { return peer_hangup_; }
    // No reuse for further exchanges once any end has closed or failed.
    [[nodiscard]] bool peer_closed() const noexcept { return eof_ || peer_hangup_ || terminal_; }
    [[nodiscard]] bool terminal() const noexcept { return terminal_; }
    [[nodiscard]] fiber::common::IoErr terminal_error() const noexcept { return terminal_error_; }
    int release_fd() noexcept;
    void close();

    fiber::common::IoErr set_read_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr set_write_callback(ReadyCallback callback, void *ctx) noexcept;
    // Terminal notifications are one-shot and never invoked inline by the
    // setter. A late subscription on an already terminal stream observes the
    // recorded outcome through a loop-local queued completion.
    fiber::common::IoErr set_terminal_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr clear_read_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr clear_write_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr clear_terminal_callback(ReadyCallback callback, void *ctx) noexcept;

    // Idle-pool observation: ensure the fd listens for the stream-state bits
    // (RDHUP; ERR/HUP arrive with any registration) without subscribing a
    // direction callback.
    fiber::common::IoErr ensure_state_observation() noexcept;

    // Loop handover, see RWFd. Requires no remaining subscriptions; stream
    // states (EOF/hangup/terminal) survive the handover.
    fiber::common::IoErr detach_for_handover() noexcept;
    fiber::common::IoErr adopt_loop(fiber::event::EventLoop &loop) noexcept;

    [[nodiscard]] IoTask read(void *buf, size_t len,
                              std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] IoTask write(const void *buf, size_t len,
                               std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] IoTask readv(const struct iovec *iov, int iovcnt,
                               std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] IoTask writev(const struct iovec *iov, int iovcnt,
                                std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] WaitReadableAwaiter
    wait_readable(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] WaitWritableAwaiter
    wait_writable(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_read(void *buf, size_t len) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_write(const void *buf, size_t len) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_readv(const struct iovec *iov, int iovcnt) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_writev(const struct iovec *iov, int iovcnt) noexcept;

private:
    // One syscall each; the fd arrives from the RWFd I/O wrapper.
    fiber::common::IoErr read_once(int socket_fd, void *buf, size_t len, size_t &out) noexcept;
    fiber::common::IoErr write_once(int socket_fd, const void *buf, size_t len, size_t &out) noexcept;
    fiber::common::IoErr readv_once(int socket_fd, const struct iovec *iov, int iovcnt, size_t &out) noexcept;
    fiber::common::IoErr writev_once(int socket_fd, const struct iovec *iov, int iovcnt, size_t &out) noexcept;

    // Marks stream state after one direction's syscall result and feeds the
    // direction readiness back through `state`.
    void finish_stream_read(RWFd::IoStateUpdate &state, fiber::common::IoErr err, size_t out, size_t len) noexcept;
    void finish_stream_write(RWFd::IoStateUpdate &state, fiber::common::IoErr err) noexcept;

    void mark_fatal(fiber::common::IoErr error) noexcept;
    void queue_terminal_notification() noexcept;
    // One-shot dispatch with a destroyed-observer: the business callback may
    // destroy this StreamFd, after which no member may be touched.
    void run_terminal_notification() noexcept;

    static void on_rwfd_stream_event(void *ctx, fiber::event::IoEvent events) noexcept;
    static fiber::common::IoResult<bool> on_rwfd_wait_gate(void *ctx, fiber::event::IoEvent direction) noexcept;

    // Veto passed into every RWFd wait this adapter starts: known stream
    // states must not park on a further readiness edge.
    [[nodiscard]] RWFd::StreamWaitGate stream_wait_gate() noexcept { return {&StreamFd::on_rwfd_wait_gate, this}; }
    static void on_deferred_terminal_notify(StreamFd *self) noexcept;

    RWFd rwfd_;
    bool eof_ = false;
    bool peer_hangup_ = false;
    bool terminal_ = false;
    fiber::common::IoErr terminal_error_ = fiber::common::IoErr::None;
    ReadyCallback terminal_callback_ = nullptr;
    void *terminal_callback_ctx_ = nullptr;
    // Destroyed-observer of the active terminal notification.
    bool *terminal_dispatch_observer_ = nullptr;
    fiber::event::EventLoop::DeferEntry terminal_notify_entry_{};
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_STREAM_FD_H
