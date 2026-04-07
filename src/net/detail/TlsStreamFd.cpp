#include "TlsStreamFd.h"

#include <cerrno>

#include "../../common/Assert.h"

#include <openssl/ssl.h>

namespace fiber::net::detail {

namespace {

template<typename ReadWaiter, typename WriteWaiter>
fiber::common::IoResult<void> resume_waiter(ReadWaiter &read_waiter, WriteWaiter &write_waiter) noexcept {
    if (read_waiter) {
        auto result = read_waiter->await_resume();
        read_waiter.reset();
        return result;
    }
    if (write_waiter) {
        auto result = write_waiter->await_resume();
        write_waiter.reset();
        return result;
    }
    return std::unexpected(fiber::common::IoErr::Invalid);
}

template<typename ReadWaiter, typename WriteWaiter>
void cancel_waiter(ReadWaiter &read_waiter, WriteWaiter &write_waiter) noexcept {
    read_waiter.reset();
    write_waiter.reset();
}

struct BusyResetGuard {
    bool *busy = nullptr;

    explicit BusyResetGuard(bool *value) noexcept : busy(value) {}

    BusyResetGuard(const BusyResetGuard &) = delete;
    BusyResetGuard &operator=(const BusyResetGuard &) = delete;

    ~BusyResetGuard() {
        if (busy) {
            *busy = false;
        }
    }
};

} // namespace

TlsStreamFd::TlsStreamFd(fiber::event::EventLoop &loop, int fd) : stream_fd_(loop, fd) {}

TlsStreamFd::~TlsStreamFd() {
    if (!stream_fd_.valid() && ssl_ == nullptr) {
        return;
    }
    if (loop().in_loop()) {
        close();
        return;
    }
    FIBER_ASSERT(false);
}

common::IoResult<void> TlsStreamFd::init(SSL_CTX *ctx, bool is_server, ConfigureSslFn configure_ssl,
                                         void *configure_ssl_ctx) {
    if (!ctx) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (!stream_fd_.valid()) {
        return std::unexpected(common::IoErr::BadFd);
    }
    if (ssl_) {
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    ssl_ = SSL_new(ctx);
    if (!ssl_) {
        return std::unexpected(common::IoErr::NoMem);
    }
    if (SSL_set_fd(ssl_, stream_fd_.fd()) != 1) {
        SSL_free(ssl_);
        ssl_ = nullptr;
        return std::unexpected(common::IoErr::Invalid);
    }
    if (is_server) {
        SSL_set_accept_state(ssl_);
    } else {
        SSL_set_connect_state(ssl_);
    }
    if (configure_ssl) {
        configure_ssl(ssl_, configure_ssl_ctx);
    }
    handshake_done_ = false;
    return {};
}

bool TlsStreamFd::valid() const noexcept { return stream_fd_.valid() && ssl_ != nullptr; }

int TlsStreamFd::fd() const noexcept { return stream_fd_.fd(); }

fiber::event::EventLoop &TlsStreamFd::loop() const noexcept { return stream_fd_.loop(); }

std::string TlsStreamFd::selected_alpn() const noexcept {
    if (!ssl_) {
        return {};
    }
    const unsigned char *proto = nullptr;
    unsigned int proto_len = 0;
    SSL_get0_alpn_selected(ssl_, &proto, &proto_len);
    if (!proto || proto_len == 0) {
        return {};
    }
    return std::string(reinterpret_cast<const char *>(proto), proto_len);
}

bool TlsStreamFd::handshake_done() const noexcept { return handshake_done_; }

void TlsStreamFd::close() {
    FIBER_ASSERT(loop().in_loop());
    if (ssl_) {
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
        ssl_ = nullptr;
        handshake_done_ = false;
    }
    if (stream_fd_.valid()) {
        stream_fd_.close();
    }
    busy_ = false;
}

TlsStreamFd::ReadAwaiter TlsStreamFd::read(void *buf, size_t len) noexcept { return {*this, buf, len}; }

TlsStreamFd::WriteAwaiter TlsStreamFd::write(const void *buf, size_t len) noexcept { return {*this, buf, len}; }

fiber::common::IoResult<size_t> TlsStreamFd::try_read(void *buf, size_t len) noexcept {
    if (busy_) {
        return std::unexpected(fiber::common::IoErr::Busy);
    }
    busy_ = true;
    size_t out = 0;
    fiber::event::IoEvent wait_event = fiber::event::IoEvent::None;
    fiber::common::IoErr err = read_once(buf, len, out, wait_event);
    busy_ = false;
    if (err == fiber::common::IoErr::WouldBlock && wait_event == fiber::event::IoEvent::None) {
        return std::unexpected(fiber::common::IoErr::Invalid);
    }
    if (err == fiber::common::IoErr::None) {
        return out;
    }
    return std::unexpected(err);
}

fiber::common::IoResult<size_t> TlsStreamFd::try_write(const void *buf, size_t len) noexcept {
    if (busy_) {
        return std::unexpected(fiber::common::IoErr::Busy);
    }
    busy_ = true;
    size_t out = 0;
    fiber::event::IoEvent wait_event = fiber::event::IoEvent::None;
    fiber::common::IoErr err = write_once(buf, len, out, wait_event);
    busy_ = false;
    if (err == fiber::common::IoErr::WouldBlock && wait_event == fiber::event::IoEvent::None) {
        return std::unexpected(fiber::common::IoErr::Invalid);
    }
    if (err == fiber::common::IoErr::None) {
        return out;
    }
    return std::unexpected(err);
}

TlsStreamFd::HandshakeTask TlsStreamFd::handshake() {
    if (busy_) {
        co_return std::unexpected(fiber::common::IoErr::Busy);
    }

    busy_ = true;
    BusyResetGuard busy_reset(&busy_);
    for (;;) {
        fiber::event::IoEvent wait_event = fiber::event::IoEvent::None;
        fiber::common::IoErr err = handshake_once(wait_event);
        if (err == fiber::common::IoErr::None) {
            co_return fiber::common::IoResult<void>{};
        }
        if (err != fiber::common::IoErr::WouldBlock) {
            co_return std::unexpected(err);
        }
        if (wait_event == fiber::event::IoEvent::Read) {
            auto wait_result = co_await stream_fd_.wait_readable();
            if (!wait_result) {
                co_return std::unexpected(wait_result.error());
            }
            continue;
        }
        if (wait_event == fiber::event::IoEvent::Write) {
            auto wait_result = co_await stream_fd_.wait_writable();
            if (!wait_result) {
                co_return std::unexpected(wait_result.error());
            }
            continue;
        }
        co_return std::unexpected(fiber::common::IoErr::Invalid);
    }
}

TlsStreamFd::ShutdownTask TlsStreamFd::shutdown() {
    if (busy_) {
        co_return std::unexpected(fiber::common::IoErr::Busy);
    }

    busy_ = true;
    BusyResetGuard busy_reset(&busy_);
    for (;;) {
        fiber::event::IoEvent wait_event = fiber::event::IoEvent::None;
        fiber::common::IoErr err = shutdown_once(wait_event);
        if (err == fiber::common::IoErr::None) {
            co_return fiber::common::IoResult<void>{};
        }
        if (err != fiber::common::IoErr::WouldBlock) {
            co_return std::unexpected(err);
        }
        if (wait_event == fiber::event::IoEvent::Read) {
            auto wait_result = co_await stream_fd_.wait_readable();
            if (!wait_result) {
                co_return std::unexpected(wait_result.error());
            }
            continue;
        }
        if (wait_event == fiber::event::IoEvent::Write) {
            auto wait_result = co_await stream_fd_.wait_writable();
            if (!wait_result) {
                co_return std::unexpected(wait_result.error());
            }
            continue;
        }
        co_return std::unexpected(fiber::common::IoErr::Invalid);
    }
}

StreamFd::WaitReadableAwaiter TlsStreamFd::wait_readable() noexcept { return stream_fd_.wait_readable(); }

StreamFd::WaitWritableAwaiter TlsStreamFd::wait_writable() noexcept { return stream_fd_.wait_writable(); }

fiber::common::IoErr TlsStreamFd::poll_handshake(fiber::event::IoEvent &event) noexcept {
    return handshake_once(event);
}

fiber::common::IoErr TlsStreamFd::poll_shutdown(fiber::event::IoEvent &event) noexcept { return shutdown_once(event); }

fiber::common::IoErr TlsStreamFd::poll_read(void *buf, size_t len, size_t &out, fiber::event::IoEvent &event) noexcept {
    return read_once(buf, len, out, event);
}

fiber::common::IoErr TlsStreamFd::poll_write(const void *buf, size_t len, size_t &out,
                                             fiber::event::IoEvent &event) noexcept {
    return write_once(buf, len, out, event);
}

fiber::common::IoErr TlsStreamFd::handshake_once(fiber::event::IoEvent &event) noexcept {
    if (!stream_fd_.valid() || !ssl_) {
        return fiber::common::IoErr::BadFd;
    }
    if (handshake_done_) {
        return fiber::common::IoErr::None;
    }
    for (;;) {
        int rc = SSL_do_handshake(ssl_);
        if (rc == 1) {
            handshake_done_ = true;
            return fiber::common::IoErr::None;
        }
        int err = SSL_get_error(ssl_, rc);
        if (err == SSL_ERROR_WANT_READ) {
            event = fiber::event::IoEvent::Read;
            return fiber::common::IoErr::WouldBlock;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            event = fiber::event::IoEvent::Write;
            return fiber::common::IoErr::WouldBlock;
        }
        if (err == SSL_ERROR_ZERO_RETURN) {
            return fiber::common::IoErr::ConnReset;
        }
        if (err == SSL_ERROR_SYSCALL) {
            int sys_err = errno;
            if (sys_err == EINTR) {
                continue;
            }
            if (sys_err != 0) {
                return fiber::common::io_err_from_errno(sys_err);
            }
            return fiber::common::IoErr::ConnReset;
        }
        return fiber::common::IoErr::Invalid;
    }
}

fiber::common::IoErr TlsStreamFd::shutdown_once(fiber::event::IoEvent &event) noexcept {
    if (!stream_fd_.valid() || !ssl_) {
        return fiber::common::IoErr::BadFd;
    }
    if (!handshake_done_) {
        return fiber::common::IoErr::None;
    }
    for (;;) {
        int rc = SSL_shutdown(ssl_);
        if (rc == 1 || rc == 0) {
            return fiber::common::IoErr::None;
        }
        int err = SSL_get_error(ssl_, rc);
        if (err == SSL_ERROR_WANT_READ) {
            event = fiber::event::IoEvent::Read;
            return fiber::common::IoErr::WouldBlock;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            event = fiber::event::IoEvent::Write;
            return fiber::common::IoErr::WouldBlock;
        }
        if (err == SSL_ERROR_ZERO_RETURN) {
            return fiber::common::IoErr::None;
        }
        if (err == SSL_ERROR_SYSCALL) {
            int sys_err = errno;
            if (sys_err == EINTR) {
                continue;
            }
            if (sys_err != 0) {
                return fiber::common::io_err_from_errno(sys_err);
            }
            return fiber::common::IoErr::Invalid;
        }
        return fiber::common::IoErr::Invalid;
    }
}

fiber::common::IoErr TlsStreamFd::read_once(void *buf, size_t len, size_t &out, fiber::event::IoEvent &event) noexcept {
    out = 0;
    if (!stream_fd_.valid() || !ssl_) {
        return fiber::common::IoErr::BadFd;
    }
    for (;;) {
        int rc = SSL_read(ssl_, buf, static_cast<int>(len));
        if (rc > 0) {
            out = static_cast<size_t>(rc);
            return fiber::common::IoErr::None;
        }
        int err = SSL_get_error(ssl_, rc);
        if (err == SSL_ERROR_WANT_READ) {
            event = fiber::event::IoEvent::Read;
            return fiber::common::IoErr::WouldBlock;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            event = fiber::event::IoEvent::Write;
            return fiber::common::IoErr::WouldBlock;
        }
        if (err == SSL_ERROR_ZERO_RETURN) {
            return fiber::common::IoErr::None;
        }
        if (err == SSL_ERROR_SYSCALL) {
            int sys_err = errno;
            if (sys_err == EINTR) {
                continue;
            }
            if (sys_err != 0) {
                return fiber::common::io_err_from_errno(sys_err);
            }
            return fiber::common::IoErr::ConnReset;
        }
        return fiber::common::IoErr::Invalid;
    }
}

fiber::common::IoErr TlsStreamFd::write_once(const void *buf, size_t len, size_t &out,
                                             fiber::event::IoEvent &event) noexcept {
    out = 0;
    if (!stream_fd_.valid() || !ssl_) {
        return fiber::common::IoErr::BadFd;
    }
    for (;;) {
        int rc = SSL_write(ssl_, buf, static_cast<int>(len));
        if (rc > 0) {
            out = static_cast<size_t>(rc);
            return fiber::common::IoErr::None;
        }
        int err = SSL_get_error(ssl_, rc);
        if (err == SSL_ERROR_WANT_READ) {
            event = fiber::event::IoEvent::Read;
            return fiber::common::IoErr::WouldBlock;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            event = fiber::event::IoEvent::Write;
            return fiber::common::IoErr::WouldBlock;
        }
        if (err == SSL_ERROR_ZERO_RETURN) {
            return fiber::common::IoErr::BrokenPipe;
        }
        if (err == SSL_ERROR_SYSCALL) {
            int sys_err = errno;
            if (sys_err == EINTR) {
                continue;
            }
            if (sys_err != 0) {
                return fiber::common::io_err_from_errno(sys_err);
            }
            return fiber::common::IoErr::BrokenPipe;
        }
        return fiber::common::IoErr::Invalid;
    }
}

TlsStreamFd::ReadAwaiter::ReadAwaiter(TlsStreamFd &stream, void *buf, size_t len) noexcept :
    stream_(&stream), buf_(buf), len_(len) {}

TlsStreamFd::ReadAwaiter::~ReadAwaiter() {
    if (!waiting_) {
        return;
    }
    cancel_waiter(read_waiter_, write_waiter_);
    waiting_ = false;
    stream_->busy_ = false;
}

bool TlsStreamFd::ReadAwaiter::await_suspend(std::coroutine_handle<> handle) {
    err_ = fiber::common::IoErr::None;
    completed_ = false;
    if (stream_->busy_) {
        err_ = fiber::common::IoErr::Busy;
        completed_ = true;
        return false;
    }

    stream_->busy_ = true;
    size_t out = 0;
    fiber::event::IoEvent wait_event = fiber::event::IoEvent::None;
    fiber::common::IoErr err = stream_->read_once(buf_, len_, out, wait_event);
    result_ = out;
    if (err == fiber::common::IoErr::None) {
        stream_->busy_ = false;
        completed_ = true;
        return false;
    }
    if (err != fiber::common::IoErr::WouldBlock) {
        stream_->busy_ = false;
        err_ = err;
        completed_ = true;
        return false;
    }
    waiting_ = true;
    if (wait_event == fiber::event::IoEvent::Read) {
        read_waiter_.emplace(stream_->stream_fd_.rwfd());
        return read_waiter_->await_suspend(handle);
    }
    if (wait_event == fiber::event::IoEvent::Write) {
        write_waiter_.emplace(stream_->stream_fd_.rwfd());
        return write_waiter_->await_suspend(handle);
    }

    waiting_ = false;
    stream_->busy_ = false;
    err_ = fiber::common::IoErr::Invalid;
    completed_ = true;
    return false;
}

fiber::common::IoResult<size_t> TlsStreamFd::ReadAwaiter::await_resume() noexcept {
    if (completed_) {
        completed_ = false;
        if (err_ == fiber::common::IoErr::None) {
            return result_;
        }
        return std::unexpected(err_);
    }

    waiting_ = false;
    fiber::common::IoResult<void> wait_result = resume_waiter(read_waiter_, write_waiter_);
    if (!wait_result) {
        stream_->busy_ = false;
        return std::unexpected(wait_result.error());
    }

    size_t out = 0;
    fiber::event::IoEvent wait_event = fiber::event::IoEvent::None;
    fiber::common::IoErr err = stream_->read_once(buf_, len_, out, wait_event);
    stream_->busy_ = false;
    if (err == fiber::common::IoErr::None) {
        return out;
    }
    if (err == fiber::common::IoErr::WouldBlock && wait_event == fiber::event::IoEvent::None) {
        return std::unexpected(fiber::common::IoErr::Invalid);
    }
    return std::unexpected(err);
}

TlsStreamFd::WriteAwaiter::WriteAwaiter(TlsStreamFd &stream, const void *buf, size_t len) noexcept :
    stream_(&stream), buf_(buf), len_(len) {}

TlsStreamFd::WriteAwaiter::~WriteAwaiter() {
    if (!waiting_) {
        return;
    }
    cancel_waiter(read_waiter_, write_waiter_);
    waiting_ = false;
    stream_->busy_ = false;
}

bool TlsStreamFd::WriteAwaiter::await_suspend(std::coroutine_handle<> handle) {
    err_ = fiber::common::IoErr::None;
    completed_ = false;
    if (stream_->busy_) {
        err_ = fiber::common::IoErr::Busy;
        completed_ = true;
        return false;
    }

    stream_->busy_ = true;
    size_t out = 0;
    fiber::event::IoEvent wait_event = fiber::event::IoEvent::None;
    fiber::common::IoErr err = stream_->write_once(buf_, len_, out, wait_event);
    result_ = out;
    if (err == fiber::common::IoErr::None) {
        stream_->busy_ = false;
        completed_ = true;
        return false;
    }
    if (err != fiber::common::IoErr::WouldBlock) {
        stream_->busy_ = false;
        err_ = err;
        completed_ = true;
        return false;
    }
    waiting_ = true;
    if (wait_event == fiber::event::IoEvent::Read) {
        read_waiter_.emplace(stream_->stream_fd_.rwfd());
        return read_waiter_->await_suspend(handle);
    }
    if (wait_event == fiber::event::IoEvent::Write) {
        write_waiter_.emplace(stream_->stream_fd_.rwfd());
        return write_waiter_->await_suspend(handle);
    }

    waiting_ = false;
    stream_->busy_ = false;
    err_ = fiber::common::IoErr::Invalid;
    completed_ = true;
    return false;
}

fiber::common::IoResult<size_t> TlsStreamFd::WriteAwaiter::await_resume() noexcept {
    if (completed_) {
        completed_ = false;
        if (err_ == fiber::common::IoErr::None) {
            return result_;
        }
        return std::unexpected(err_);
    }

    waiting_ = false;
    fiber::common::IoResult<void> wait_result = resume_waiter(read_waiter_, write_waiter_);
    if (!wait_result) {
        stream_->busy_ = false;
        return std::unexpected(wait_result.error());
    }

    size_t out = 0;
    fiber::event::IoEvent wait_event = fiber::event::IoEvent::None;
    fiber::common::IoErr err = stream_->write_once(buf_, len_, out, wait_event);
    stream_->busy_ = false;
    if (err == fiber::common::IoErr::None) {
        return out;
    }
    if (err == fiber::common::IoErr::WouldBlock && wait_event == fiber::event::IoEvent::None) {
        return std::unexpected(fiber::common::IoErr::Invalid);
    }
    return std::unexpected(err);
}

} // namespace fiber::net::detail
