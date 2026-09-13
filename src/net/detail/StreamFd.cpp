#include <fiber/net/detail/StreamFd.h>

#include <cerrno>
#include <climits>
#include <limits>
#include <sys/socket.h>
#include <sys/uio.h>

#include <fiber/common/Assert.h>

namespace fiber::net::detail {
namespace {

using Deadline = std::chrono::steady_clock::time_point;

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

} // namespace

StreamFd::StreamFd(fiber::event::EventLoop &loop, int fd) : rwfd_(loop, fd, RWFd::Kind::Stream) {}

StreamFd::~StreamFd() {
    if (!rwfd_.valid()) {
        return;
    }
    if (rwfd_.loop().in_loop()) {
        close();
        return;
    }
    FIBER_ASSERT(false);
}

bool StreamFd::valid() const noexcept { return rwfd_.valid(); }

int StreamFd::fd() const noexcept { return rwfd_.fd(); }

fiber::event::EventLoop &StreamFd::loop() const noexcept { return rwfd_.loop(); }

RWFd &StreamFd::rwfd() noexcept { return rwfd_; }

int StreamFd::release_fd() noexcept { return rwfd_.release_fd(); }

void StreamFd::close() { rwfd_.close(); }

fiber::common::IoErr StreamFd::set_read_callback(ReadyCallback callback, void *ctx) noexcept {
    return rwfd_.set_read_callback(callback, ctx);
}

fiber::common::IoErr StreamFd::set_write_callback(ReadyCallback callback, void *ctx) noexcept {
    return rwfd_.set_write_callback(callback, ctx);
}

fiber::common::IoErr StreamFd::set_terminal_callback(ReadyCallback callback, void *ctx) noexcept {
    return rwfd_.set_terminal_callback(callback, ctx);
}

fiber::common::IoErr StreamFd::clear_read_callback(ReadyCallback callback, void *ctx) noexcept {
    return rwfd_.clear_read_callback(callback, ctx);
}

fiber::common::IoErr StreamFd::clear_write_callback(ReadyCallback callback, void *ctx) noexcept {
    return rwfd_.clear_write_callback(callback, ctx);
}

fiber::common::IoErr StreamFd::clear_terminal_callback(ReadyCallback callback, void *ctx) noexcept {
    return rwfd_.clear_terminal_callback(callback, ctx);
}

StreamFd::IoTask StreamFd::read(void *buf, size_t len, std::chrono::milliseconds timeout) noexcept {
    Deadline deadline = make_deadline(timeout);
    for (;;) {
        auto result = try_read(buf, len);
        if (result || result.error() != fiber::common::IoErr::WouldBlock) {
            co_return result;
        }
        auto remaining = remaining_timeout(deadline);
        if (!remaining) {
            co_return std::unexpected(remaining.error());
        }
        auto wait_result = co_await rwfd_.wait_readable(*remaining);
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
        auto wait_result = co_await rwfd_.wait_writable(*remaining);
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
}

StreamFd::IoTask StreamFd::readv(const struct iovec *iov, int iovcnt, std::chrono::milliseconds timeout) noexcept {
    Deadline deadline = make_deadline(timeout);
    for (;;) {
        auto result = try_readv(iov, iovcnt);
        if (result || result.error() != fiber::common::IoErr::WouldBlock) {
            co_return result;
        }
        auto remaining = remaining_timeout(deadline);
        if (!remaining) {
            co_return std::unexpected(remaining.error());
        }
        auto wait_result = co_await rwfd_.wait_readable(*remaining);
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
        auto wait_result = co_await rwfd_.wait_writable(*remaining);
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
}

StreamFd::WaitReadableAwaiter StreamFd::wait_readable(std::chrono::milliseconds timeout) noexcept {
    return rwfd_.wait_readable(timeout);
}

StreamFd::WaitWritableAwaiter StreamFd::wait_writable(std::chrono::milliseconds timeout) noexcept {
    return rwfd_.wait_writable(timeout);
}

fiber::common::IoResult<size_t> StreamFd::try_read(void *buf, size_t len) noexcept {
    if (!valid()) {
        return std::unexpected(common::IoErr::BadFd);
    }
    if (len != 0 && !buf) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (len == 0) {
        return 0;
    }
    auto err = rwfd_.prepare_io(event::IoEvent::Read);
    if (err != common::IoErr::None) {
        return std::unexpected(err);
    }
    size_t out = 0;
    err = read_once(buf, len, out);
    rwfd_.finish_io(event::IoEvent::Read, err, err == common::IoErr::None && out > 0 && out < len && len <= 0x7ffff000);
    if (err != common::IoErr::None) {
        return std::unexpected(err);
    }
    return out;
}

fiber::common::IoResult<size_t> StreamFd::try_write(const void *buf, size_t len) noexcept {
    if (!valid()) {
        return std::unexpected(common::IoErr::BadFd);
    }
    if (len != 0 && !buf) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (len == 0) {
        return 0;
    }
    auto err = rwfd_.prepare_io(event::IoEvent::Write);
    if (err != common::IoErr::None) {
        return std::unexpected(err);
    }
    size_t out = 0;
    err = write_once(buf, len, out);
    rwfd_.finish_io(event::IoEvent::Write, err, false);
    if (err != common::IoErr::None) {
        return std::unexpected(err);
    }
    return out;
}

fiber::common::IoResult<size_t> StreamFd::try_readv(const struct iovec *iov, int iovcnt) noexcept {
    if (!valid()) {
        return std::unexpected(common::IoErr::BadFd);
    }
    if (iovcnt < 0 || iovcnt > IOV_MAX || (iovcnt != 0 && !iov)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    size_t len = 0;
    for (int i = 0; i < iovcnt; ++i) {
        if (iov[i].iov_len > static_cast<size_t>(SSIZE_MAX) - len || (iov[i].iov_len && !iov[i].iov_base)) {
            return std::unexpected(common::IoErr::Invalid);
        }
        len += iov[i].iov_len;
    }
    if (len == 0) {
        return 0;
    }
    auto err = rwfd_.prepare_io(event::IoEvent::Read);
    if (err != common::IoErr::None) {
        return std::unexpected(err);
    }
    size_t out = 0;
    err = readv_once(iov, iovcnt, out);
    rwfd_.finish_io(event::IoEvent::Read, err, err == common::IoErr::None && out > 0 && out < len && len <= 0x7ffff000);
    if (err != common::IoErr::None) {
        return std::unexpected(err);
    }
    return out;
}

fiber::common::IoResult<size_t> StreamFd::try_writev(const struct iovec *iov, int iovcnt) noexcept {
    if (!valid()) {
        return std::unexpected(common::IoErr::BadFd);
    }
    if (iovcnt < 0 || iovcnt > IOV_MAX || (iovcnt != 0 && !iov)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    size_t len = 0;
    for (int i = 0; i < iovcnt; ++i) {
        if (iov[i].iov_len > static_cast<size_t>(SSIZE_MAX) - len || (iov[i].iov_len && !iov[i].iov_base)) {
            return std::unexpected(common::IoErr::Invalid);
        }
        len += iov[i].iov_len;
    }
    if (len == 0) {
        return 0;
    }
    auto err = rwfd_.prepare_io(event::IoEvent::Write);
    if (err != common::IoErr::None) {
        return std::unexpected(err);
    }
    size_t out = 0;
    err = writev_once(iov, iovcnt, out);
    rwfd_.finish_io(event::IoEvent::Write, err, false);
    if (err != common::IoErr::None) {
        return std::unexpected(err);
    }
    return out;
}

fiber::common::IoErr StreamFd::read_once(void *buf, size_t len, size_t &out) noexcept {
    out = 0;
    int socket_fd = rwfd_.fd();
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

fiber::common::IoErr StreamFd::write_once(const void *buf, size_t len, size_t &out) noexcept {
    out = 0;
    int socket_fd = rwfd_.fd();
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

fiber::common::IoErr StreamFd::readv_once(const struct iovec *iov, int iovcnt, size_t &out) noexcept {
    out = 0;
    int socket_fd = rwfd_.fd();
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

fiber::common::IoErr StreamFd::writev_once(const struct iovec *iov, int iovcnt, size_t &out) noexcept {
    out = 0;
    int socket_fd = rwfd_.fd();
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
