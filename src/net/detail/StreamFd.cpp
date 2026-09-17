#include <fiber/net/detail/StreamFd.h>

#include <cerrno>
#include <climits>
#include <limits>
#include <sys/socket.h>
#include <sys/uio.h>
#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::net::detail {
namespace {

using Deadline = std::chrono::steady_clock::time_point;

// Largest request length a stream syscall can prove exhausted by a short
// result; above it a short read only proves a kernel transfer limit.
constexpr size_t kMaxProvableStreamRequest = 0x7ffff000;

Deadline make_deadline(std::chrono::milliseconds timeout) noexcept {
    if (timeout == std::chrono::milliseconds::max()) {
        return Deadline::max();
    }
    return fiber::event::EventLoop::current().now() + timeout;
}

fiber::common::IoResult<std::chrono::milliseconds> remaining_timeout(Deadline deadline) noexcept {
    if (deadline == Deadline::max()) {
        return std::chrono::milliseconds::max();
    }
    auto now = fiber::event::EventLoop::current().now();
    if (deadline <= now) {
        return std::unexpected(fiber::common::IoErr::TimedOut);
    }
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    if (remaining <= std::chrono::milliseconds::zero()) {
        remaining = std::chrono::milliseconds(1);
    }
    return remaining;
}

ssize_t send_no_sigpipe(int fd, const void *buf, size_t len) noexcept {
#ifdef MSG_NOSIGNAL
    return ::send(fd, buf, len, MSG_NOSIGNAL | MSG_DONTWAIT);
#else
    return ::send(fd, buf, len, MSG_DONTWAIT);
#endif
}

ssize_t sendv_no_sigpipe(int fd, const struct iovec *iov, int iovcnt) noexcept {
#ifdef MSG_NOSIGNAL
    struct msghdr msg{};
    msg.msg_iov = const_cast<struct iovec *>(iov);
    msg.msg_iovlen = static_cast<decltype(msg.msg_iovlen)>(iovcnt);
    return ::sendmsg(fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
#else
    return ::writev(fd, iov, iovcnt);
#endif
}

bool is_fatal_stream_error(fiber::common::IoErr err) noexcept {
    switch (err) {
        case fiber::common::IoErr::ConnReset:
        case fiber::common::IoErr::BrokenPipe:
        case fiber::common::IoErr::NotConnected:
        case fiber::common::IoErr::ConnAborted:
            return true;
        default:
            return false;
    }
}

} // namespace

StreamFd::StreamFd(fiber::event::EventLoop &owner_loop, int fd) : rwfd_(owner_loop, fd, RWFd::Kind::Stream) {
    rwfd_.set_stream_event_sink(this, &StreamFd::on_rwfd_stream_event);
}

StreamFd::~StreamFd() {
    if (terminal_dispatch_observer_) {
        *terminal_dispatch_observer_ = true;
        terminal_dispatch_observer_ = nullptr;
    }
    if (rwfd_.current_loop().in_loop()) {
        close();
        return;
    }
    // Off-loop destruction is legal only for a detached object: the handover
    // protocol adopts it on a loop and closes it there, so nothing may remain
    // subscribed or queued here.
    FIBER_ASSERT(!rwfd_.registered() && terminal_callback_ == nullptr && !terminal_notify_entry_.is_in_queue());
}

bool StreamFd::valid() const noexcept { return rwfd_.valid(); }

int StreamFd::fd() const noexcept { return rwfd_.fd(); }

fiber::event::EventLoop &StreamFd::owner_loop() const noexcept { return rwfd_.owner_loop(); }

fiber::event::EventLoop &StreamFd::current_loop() const noexcept { return rwfd_.current_loop(); }

fiber::event::EventLoop &StreamFd::loop() const noexcept { return rwfd_.current_loop(); }

RWFd &StreamFd::rwfd() noexcept { return rwfd_; }

int StreamFd::release_fd() noexcept { return rwfd_.release_fd(); }

void StreamFd::close() {
    FIBER_ASSERT(rwfd_.current_loop().in_loop());
    if (terminal_notify_entry_.is_in_queue()) {
        rwfd_.current_loop().cancel<StreamFd, &StreamFd::terminal_notify_entry_>(*this);
    }
    const auto terminal_callback = std::exchange(terminal_callback_, nullptr);
    void *terminal_ctx = std::exchange(terminal_callback_ctx_, nullptr);
    if (!terminal_) {
        terminal_ = true;
        terminal_error_ = fiber::common::IoErr::Canceled;
    }
    const auto error = terminal_error_;
    // Teardown detaches every subscription first; the completions below may
    // destroy this object, so only locals are used afterwards.
    auto detached = rwfd_.detach_for_close();
    if (terminal_callback) {
        terminal_callback(terminal_ctx, error);
    }
    detached.complete(fiber::common::IoErr::Canceled);
}

fiber::common::IoErr StreamFd::set_read_callback(ReadyCallback callback, void *ctx) noexcept {
    return rwfd_.set_read_callback(callback, ctx);
}

fiber::common::IoErr StreamFd::set_write_callback(ReadyCallback callback, void *ctx) noexcept {
    return rwfd_.set_write_callback(callback, ctx);
}

fiber::common::IoErr StreamFd::set_terminal_callback(ReadyCallback callback, void *ctx) noexcept {
    FIBER_ASSERT(rwfd_.current_loop().in_loop());
    if (!callback) {
        return fiber::common::IoErr::Invalid;
    }
    if (terminal_callback_) {
        return fiber::common::IoErr::Busy;
    }
    // ERR/HUP arrive with any registration, but RDHUP needs explicit interest.
    const auto observed = ensure_state_observation();
    if (observed != fiber::common::IoErr::None) {
        return observed;
    }
    terminal_callback_ = callback;
    terminal_callback_ctx_ = ctx;
    if (terminal_) {
        // Late subscription: never invoked inline; the one-shot completion is
        // queued by StreamFd itself, restoring no deferred-ready machinery in
        // RWFd.
        queue_terminal_notification();
    }
    return fiber::common::IoErr::None;
}

fiber::common::IoErr StreamFd::clear_read_callback(ReadyCallback callback, void *ctx) noexcept {
    return rwfd_.clear_read_callback(callback, ctx);
}

fiber::common::IoErr StreamFd::clear_write_callback(ReadyCallback callback, void *ctx) noexcept {
    return rwfd_.clear_write_callback(callback, ctx);
}

fiber::common::IoErr StreamFd::clear_terminal_callback(ReadyCallback callback, void *ctx) noexcept {
    FIBER_ASSERT(rwfd_.current_loop().in_loop());
    if (!callback) {
        return fiber::common::IoErr::Invalid;
    }
    if (terminal_callback_ != callback || terminal_callback_ctx_ != ctx) {
        return fiber::common::IoErr::None;
    }
    terminal_callback_ = nullptr;
    terminal_callback_ctx_ = nullptr;
    // A queued late-subscription completion finds an empty slot and no-ops.
    return fiber::common::IoErr::None;
}

fiber::common::IoErr StreamFd::ensure_state_observation() noexcept { return rwfd_.ensure_state_observation(); }

fiber::common::IoErr StreamFd::detach_for_handover() noexcept {
    FIBER_ASSERT(rwfd_.current_loop().in_loop());
    FIBER_ASSERT(terminal_callback_ == nullptr);
    if (terminal_notify_entry_.is_in_queue()) {
        rwfd_.current_loop().cancel<StreamFd, &StreamFd::terminal_notify_entry_>(*this);
    }
    return rwfd_.detach_for_handover();
}

fiber::common::IoErr StreamFd::adopt_loop(fiber::event::EventLoop &loop) noexcept { return rwfd_.adopt_loop(loop); }

void StreamFd::on_rwfd_stream_event(void *raw_ctx, fiber::event::IoEvent events) noexcept {
    auto *self = static_cast<StreamFd *>(raw_ctx);
    if (fiber::event::any(events & fiber::event::IoEvent::ReadHangup)) {
        // Buffered data may still read out first; only a read of 0 is EOF.
        self->peer_hangup_ = true;
    }
    if (fiber::event::any(events & fiber::event::IoEvent::Terminal) && !self->terminal_) {
        self->terminal_ = true;
        self->terminal_error_ = fiber::common::IoErr::Unknown;
        // Runs inside the RWFd dispatch guard; the business callback may
        // destroy this object, and run_terminal_notification plus this sink
        // touch no members after it in that case.
        self->run_terminal_notification();
    }
}

// Per-call wait veto handed to the RWFd wait awaiters through
// stream_wait_gate(); consults the recorded stream state before a waiter parks.
fiber::common::IoResult<bool> StreamFd::on_rwfd_wait_gate(void *raw_ctx, fiber::event::IoEvent direction) noexcept {
    auto *self = static_cast<StreamFd *>(raw_ctx);
    if (direction == fiber::event::IoEvent::Read) {
        if (self->eof_ || self->peer_hangup_) {
            // EOF is the readable end, and an observed read-side hangup means
            // reads can only return buffered data or 0 from now on: either way
            // a read wait must not park on a further edge (RDHUP was consumed
            // once, so none will come).
            return true;
        }
        if (self->terminal_) {
            if (self->terminal_error_ == fiber::common::IoErr::Unknown) {
                // Bare HUP/ERR hint: buffered data drains first and the syscall
                // reports the concrete end (0 or a real error). The hint alone
                // must not veto the read direction.
                return true;
            }
            // A recorded fatal error has its result to deliver.
            return std::unexpected(self->terminal_error_);
        }
        return false;
    }
    if (self->terminal_) {
        return std::unexpected(self->terminal_error_);
    }
    return false;
}

void StreamFd::mark_fatal(fiber::common::IoErr error) noexcept {
    if (terminal_) {
        return;
    }
    terminal_ = true;
    terminal_error_ = error == fiber::common::IoErr::None ? fiber::common::IoErr::Unknown : error;
    if (terminal_callback_ != nullptr) {
        // Discovered inside a read/write lambda or a ready callback; the
        // terminal completion must not re-enter that in-flight stack.
        queue_terminal_notification();
    }
}

void StreamFd::queue_terminal_notification() noexcept {
    FIBER_ASSERT(terminal_);
    rwfd_.current_loop()
            .post_local<StreamFd, &StreamFd::terminal_notify_entry_, &StreamFd::on_deferred_terminal_notify>(*this);
}

void StreamFd::on_deferred_terminal_notify(StreamFd *self) noexcept {
    if (!self->terminal_ || self->terminal_callback_ == nullptr) {
        return;
    }
    self->run_terminal_notification();
}

void StreamFd::run_terminal_notification() noexcept {
    FIBER_ASSERT(terminal_);
    const auto callback = std::exchange(terminal_callback_, nullptr);
    void *ctx = std::exchange(terminal_callback_ctx_, nullptr);
    if (callback == nullptr) {
        return;
    }
    bool destroyed = false;
    terminal_dispatch_observer_ = &destroyed;
    callback(ctx, terminal_error_);
    if (!destroyed) {
        terminal_dispatch_observer_ = nullptr;
    }
}

StreamFd::IoTask StreamFd::read(void *buf, size_t len, std::chrono::milliseconds timeout) noexcept {
    Deadline deadline = make_deadline(timeout);
    for (;;) {
        if (eof_) {
            // Recorded EOF stays authoritative; no syscall, no wait.
            co_return 0;
        }
        auto result = try_read(buf, len);
        if (result || result.error() != fiber::common::IoErr::WouldBlock) {
            co_return result;
        }
        auto remaining = remaining_timeout(deadline);
        if (!remaining) {
            co_return std::unexpected(remaining.error());
        }
        auto wait_result = co_await rwfd_.wait_readable(*remaining, stream_wait_gate());
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
}

StreamFd::IoTask StreamFd::write(const void *buf, size_t len, std::chrono::milliseconds timeout) noexcept {
    Deadline deadline = make_deadline(timeout);
    for (;;) {
        auto result = try_write(buf, len);
        if (result || result.error() != fiber::common::IoErr::WouldBlock) {
            co_return result;
        }
        auto remaining = remaining_timeout(deadline);
        if (!remaining) {
            co_return std::unexpected(remaining.error());
        }
        auto wait_result = co_await rwfd_.wait_writable(*remaining, stream_wait_gate());
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
}

StreamFd::IoTask StreamFd::readv(const struct iovec *iov, int iovcnt, std::chrono::milliseconds timeout) noexcept {
    Deadline deadline = make_deadline(timeout);
    for (;;) {
        if (eof_) {
            co_return 0;
        }
        auto result = try_readv(iov, iovcnt);
        if (result || result.error() != fiber::common::IoErr::WouldBlock) {
            co_return result;
        }
        auto remaining = remaining_timeout(deadline);
        if (!remaining) {
            co_return std::unexpected(remaining.error());
        }
        auto wait_result = co_await rwfd_.wait_readable(*remaining, stream_wait_gate());
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
}

StreamFd::IoTask StreamFd::writev(const struct iovec *iov, int iovcnt, std::chrono::milliseconds timeout) noexcept {
    Deadline deadline = make_deadline(timeout);
    for (;;) {
        auto result = try_writev(iov, iovcnt);
        if (result || result.error() != fiber::common::IoErr::WouldBlock) {
            co_return result;
        }
        auto remaining = remaining_timeout(deadline);
        if (!remaining) {
            co_return std::unexpected(remaining.error());
        }
        auto wait_result = co_await rwfd_.wait_writable(*remaining, stream_wait_gate());
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
}

StreamFd::WaitReadableAwaiter StreamFd::wait_readable(std::chrono::milliseconds timeout) noexcept {
    return rwfd_.wait_readable(timeout, stream_wait_gate());
}

StreamFd::WaitWritableAwaiter StreamFd::wait_writable(std::chrono::milliseconds timeout) noexcept {
    return rwfd_.wait_writable(timeout, stream_wait_gate());
}

void StreamFd::finish_stream_read(RWFd::IoStateUpdate &state, fiber::common::IoErr err, size_t out,
                                  size_t len) noexcept {
    if (err == fiber::common::IoErr::None) {
        if (out == 0) {
            // Stream EOF: recorded here, never a plain short read to wait on.
            eof_ = true;
            state.mark_ready();
        } else if (out < len && len <= kMaxProvableStreamRequest) {
            // Short stream read proves the receive side drained.
            state.mark_blocked();
        } else {
            // Full read does not confirm exhaustion.
            state.mark_ready();
        }
        return;
    }
    if (err == fiber::common::IoErr::WouldBlock) {
        state.mark_blocked();
        return;
    }
    if (is_fatal_stream_error(err)) {
        mark_fatal(err);
    }
    // A failed direction still has its result to deliver; the wait gate routes
    // later waits to the recorded stream state.
    state.mark_ready();
}

void StreamFd::finish_stream_write(RWFd::IoStateUpdate &state, fiber::common::IoErr err) noexcept {
    if (err == fiber::common::IoErr::WouldBlock) {
        state.mark_blocked();
        return;
    }
    if (is_fatal_stream_error(err)) {
        mark_fatal(err);
    }
    // Successful writes are not confirmed to have exhausted send space.
    state.mark_ready();
}

fiber::common::IoResult<size_t> StreamFd::try_read(void *buf, size_t len) noexcept {
    if (!valid()) {
        return std::unexpected(fiber::common::IoErr::BadFd);
    }
    if (len != 0 && !buf) {
        return std::unexpected(fiber::common::IoErr::Invalid);
    }
    if (len == 0) {
        return 0;
    }
    return rwfd_.read([&](int socket_fd, RWFd::IoStateUpdate &state) noexcept -> fiber::common::IoResult<size_t> {
        size_t out = 0;
        const auto err = read_once(socket_fd, buf, len, out);
        finish_stream_read(state, err, out, len);
        if (err != fiber::common::IoErr::None) {
            return std::unexpected(err);
        }
        return out;
    });
}

fiber::common::IoResult<size_t> StreamFd::try_write(const void *buf, size_t len) noexcept {
    if (!valid()) {
        return std::unexpected(fiber::common::IoErr::BadFd);
    }
    if (len != 0 && !buf) {
        return std::unexpected(fiber::common::IoErr::Invalid);
    }
    if (len == 0) {
        return 0;
    }
    return rwfd_.write([&](int socket_fd, RWFd::IoStateUpdate &state) noexcept -> fiber::common::IoResult<size_t> {
        size_t out = 0;
        const auto err = write_once(socket_fd, buf, len, out);
        finish_stream_write(state, err);
        if (err != fiber::common::IoErr::None) {
            return std::unexpected(err);
        }
        return out;
    });
}

fiber::common::IoResult<size_t> StreamFd::try_readv(const struct iovec *iov, int iovcnt) noexcept {
    if (!valid()) {
        return std::unexpected(fiber::common::IoErr::BadFd);
    }
    if (iovcnt < 0 || iovcnt > IOV_MAX || (iovcnt != 0 && !iov)) {
        return std::unexpected(fiber::common::IoErr::Invalid);
    }
    size_t len = 0;
    for (int i = 0; i < iovcnt; ++i) {
        if (iov[i].iov_len > static_cast<size_t>(SSIZE_MAX) - len || (iov[i].iov_len && !iov[i].iov_base)) {
            return std::unexpected(fiber::common::IoErr::Invalid);
        }
        len += iov[i].iov_len;
    }
    if (len == 0) {
        return 0;
    }
    return rwfd_.read([&](int socket_fd, RWFd::IoStateUpdate &state) noexcept -> fiber::common::IoResult<size_t> {
        size_t out = 0;
        const auto err = readv_once(socket_fd, iov, iovcnt, out);
        finish_stream_read(state, err, out, len);
        if (err != fiber::common::IoErr::None) {
            return std::unexpected(err);
        }
        return out;
    });
}

fiber::common::IoResult<size_t> StreamFd::try_writev(const struct iovec *iov, int iovcnt) noexcept {
    if (!valid()) {
        return std::unexpected(fiber::common::IoErr::BadFd);
    }
    if (iovcnt < 0 || iovcnt > IOV_MAX || (iovcnt != 0 && !iov)) {
        return std::unexpected(fiber::common::IoErr::Invalid);
    }
    size_t len = 0;
    for (int i = 0; i < iovcnt; ++i) {
        if (iov[i].iov_len > static_cast<size_t>(SSIZE_MAX) - len || (iov[i].iov_len && !iov[i].iov_base)) {
            return std::unexpected(fiber::common::IoErr::Invalid);
        }
        len += iov[i].iov_len;
    }
    if (len == 0) {
        return 0;
    }
    return rwfd_.write([&](int socket_fd, RWFd::IoStateUpdate &state) noexcept -> fiber::common::IoResult<size_t> {
        size_t out = 0;
        const auto err = writev_once(socket_fd, iov, iovcnt, out);
        finish_stream_write(state, err);
        if (err != fiber::common::IoErr::None) {
            return std::unexpected(err);
        }
        return out;
    });
}

fiber::common::IoErr StreamFd::read_once(int socket_fd, void *buf, size_t len, size_t &out) noexcept {
    out = 0;
    if (socket_fd < 0) {
        return fiber::common::IoErr::BadFd;
    }
    for (;;) {
        ssize_t rc = ::recv(socket_fd, buf, len, MSG_DONTWAIT);
        if (rc >= 0) {
            out = static_cast<size_t>(rc);
            return fiber::common::IoErr::None;
        }
        int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == EAGAIN || err == EWOULDBLOCK) {
            return fiber::common::IoErr::WouldBlock;
        }
        return fiber::common::io_err_from_errno(err);
    }
}

fiber::common::IoErr StreamFd::write_once(int socket_fd, const void *buf, size_t len, size_t &out) noexcept {
    out = 0;
    if (socket_fd < 0) {
        return fiber::common::IoErr::BadFd;
    }
    for (;;) {
        ssize_t rc = send_no_sigpipe(socket_fd, buf, len);
        if (rc >= 0) {
            out = static_cast<size_t>(rc);
            return fiber::common::IoErr::None;
        }
        int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == EAGAIN || err == EWOULDBLOCK) {
            return fiber::common::IoErr::WouldBlock;
        }
        return fiber::common::io_err_from_errno(err);
    }
}

fiber::common::IoErr StreamFd::readv_once(int socket_fd, const struct iovec *iov, int iovcnt, size_t &out) noexcept {
    out = 0;
    if (socket_fd < 0) {
        return fiber::common::IoErr::BadFd;
    }
    for (;;) {
        msghdr msg{};
        msg.msg_iov = const_cast<iovec *>(iov);
        msg.msg_iovlen = static_cast<size_t>(iovcnt);
        ssize_t rc = ::recvmsg(socket_fd, &msg, MSG_DONTWAIT);
        if (rc >= 0) {
            out = static_cast<size_t>(rc);
            return fiber::common::IoErr::None;
        }
        int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == EAGAIN || err == EWOULDBLOCK) {
            return fiber::common::IoErr::WouldBlock;
        }
        return fiber::common::io_err_from_errno(err);
    }
}

fiber::common::IoErr StreamFd::writev_once(int socket_fd, const struct iovec *iov, int iovcnt, size_t &out) noexcept {
    out = 0;
    if (socket_fd < 0) {
        return fiber::common::IoErr::BadFd;
    }
    for (;;) {
        ssize_t rc = sendv_no_sigpipe(socket_fd, iov, iovcnt);
        if (rc >= 0) {
            out = static_cast<size_t>(rc);
            return fiber::common::IoErr::None;
        }
        int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == EAGAIN || err == EWOULDBLOCK) {
            return fiber::common::IoErr::WouldBlock;
        }
        return fiber::common::io_err_from_errno(err);
    }
}

} // namespace fiber::net::detail
