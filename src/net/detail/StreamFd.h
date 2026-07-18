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
 * forbidden read/read and write/write in multi-coroutine, but read and write can overlap.
 * no internal read/write lock is enforced for performance.
 */
class StreamFd : public common::NonCopyable, public common::NonMovable {
public:
    using IoTask = fiber::async::Task<fiber::common::IoResult<size_t>>;
    using ReadyCallback = RWFd::ReadyCallback;
    using WaitReadableAwaiter = RWFd::WaitReadableAwaiter;
    using WaitWritableAwaiter = RWFd::WaitWritableAwaiter;

    StreamFd(fiber::event::EventLoop &loop, int fd);
    ~StreamFd();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] fiber::event::EventLoop &loop() const noexcept;
    [[nodiscard]] RWFd &rwfd() noexcept;
    int release_fd() noexcept;
    void close();

    fiber::common::IoErr set_read_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr set_write_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr clear_read_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr clear_write_callback(ReadyCallback callback, void *ctx) noexcept;

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
    fiber::common::IoErr read_once(void *buf, size_t len, size_t &out) noexcept;
    fiber::common::IoErr write_once(const void *buf, size_t len, size_t &out) noexcept;
    fiber::common::IoErr readv_once(const struct iovec *iov, int iovcnt, size_t &out) noexcept;
    fiber::common::IoErr writev_once(const struct iovec *iov, int iovcnt, size_t &out) noexcept;

    RWFd rwfd_;
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_STREAM_FD_H
