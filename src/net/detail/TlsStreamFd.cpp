#include "TlsStreamFd.h"

#include <cerrno>
#include <cstdint>
#include <sys/socket.h>

#include "../../common/Assert.h"

#include <openssl/bio.h>
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

// ---------------------------------------------------------------------------
// Custom TLS fd BIO.
//
// BoringSSL's built-in socket BIO (installed by SSL_set_fd) issues plain
// write(2) syscalls without MSG_NOSIGNAL, so a TLS write to a peer that has
// closed or reset the connection raises SIGPIPE and kills the process (e.g. the
// GrpcStreamTest.CancelMidStream crash). This BIO mirrors the built-in socket
// BIO exactly except its write callback uses ::send with MSG_NOSIGNAL, matching
// StreamFd's send() path. SIGPIPE is suppressed at the write site and EPIPE is
// returned instead, so every TLS caller (server+client, h2/h3/gRPC) is safe with
// no process-wide signal disposition changes.
//
// The fd lives in the BIO's data slot as a pointer-encoded int (BIO exposes no
// public fd field), avoiding any per-connection allocation. The BIO never owns
// the fd (BIO_NOCLOSE); TlsStreamFd owns it via stream_fd_.
//
// The three I/O callbacks have C language linkage because bio.h declares the
// BIO_meth_set_* parameters inside extern "C"; `static` keeps them TU-local.
// ---------------------------------------------------------------------------

// Mirrors BoringSSL's bio_errno_should_retry (crypto/bio/errno.cc): a -1 return
// with a transient errno is reported as a retry so SSL_get_error surfaces
// SSL_ERROR_WANT_READ/WRITE for the nonblocking event loop.
int tls_bio_should_retry(int ret) noexcept {
    if (ret != -1) {
        return 0;
    }
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ENOTCONN || errno == EPROTO ||
           errno == EINPROGRESS || errno == EALREADY;
}

int tls_bio_fd(BIO *b) noexcept { return static_cast<int>(reinterpret_cast<intptr_t>(BIO_get_data(b))); }

extern "C" {

static int tls_bio_read(BIO *b, char *out, int outl) noexcept {
    int ret = static_cast<int>(::recv(tls_bio_fd(b), out, static_cast<size_t>(outl), 0));
    BIO_clear_retry_flags(b);
    if (ret <= 0 && tls_bio_should_retry(ret) != 0) {
        BIO_set_retry_read(b);
    }
    return ret;
}

static int tls_bio_write(BIO *b, const char *in, int inl) noexcept {
    int ret = static_cast<int>(::send(tls_bio_fd(b), in, static_cast<size_t>(inl), MSG_NOSIGNAL));
    BIO_clear_retry_flags(b);
    if (ret <= 0 && tls_bio_should_retry(ret) != 0) {
        BIO_set_retry_write(b);
    }
    return ret;
}

static long tls_bio_ctrl(BIO *b, int cmd, long num, void *ptr) noexcept {
    switch (cmd) {
        case BIO_C_SET_FD: {
            int fd = *static_cast<int *>(ptr);
            BIO_set_data(b, reinterpret_cast<void *>(static_cast<intptr_t>(fd)));
            BIO_set_shutdown(b, static_cast<int>(num));
            BIO_set_init(b, 1);
            return 1;
        }
        case BIO_C_GET_FD: {
            if (BIO_get_init(b) == 0) {
                return -1;
            }
            int fd = tls_bio_fd(b);
            int *out = static_cast<int *>(ptr);
            if (out != nullptr) {
                *out = fd;
            }
            return fd;
        }
        case BIO_CTRL_GET_CLOSE:
            return BIO_get_shutdown(b);
        case BIO_CTRL_SET_CLOSE:
            BIO_set_shutdown(b, static_cast<int>(num));
            return 1;
        case BIO_CTRL_FLUSH:
            return 1;
        default:
            return 0;
    }
}

} // extern "C"

const BIO_METHOD *tls_fd_bio_method() {
    static const BIO_METHOD *method = []() -> const BIO_METHOD * {
        BIO_METHOD *m = BIO_meth_new(BIO_TYPE_FD, "fiber tls fd");
        if (m == nullptr) {
            return nullptr;
        }
        BIO_meth_set_write(m, &tls_bio_write);
        BIO_meth_set_read(m, &tls_bio_read);
        BIO_meth_set_ctrl(m, &tls_bio_ctrl);
        return m;
    }();
    return method;
}

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
    // Install a custom fd BIO (instead of SSL_set_fd's built-in socket BIO) so
    // TLS writes use ::send(MSG_NOSIGNAL) and never raise SIGPIPE on a closed
    // peer. The BIO does not own the fd (BIO_NOCLOSE); stream_fd_ does.
    const BIO_METHOD *bio_method = tls_fd_bio_method();
    BIO *bio = (bio_method != nullptr) ? BIO_new(bio_method) : nullptr;
    if (bio == nullptr) {
        SSL_free(ssl_);
        ssl_ = nullptr;
        return std::unexpected(common::IoErr::NoMem);
    }
    BIO_set_fd(bio, stream_fd_.fd(), BIO_NOCLOSE);
    SSL_set_bio(ssl_, bio, bio);
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

std::string_view TlsStreamFd::selected_alpn() const noexcept {
    if (!ssl_) {
        return {};
    }
    const unsigned char *proto = nullptr;
    unsigned int proto_len = 0;
    SSL_get0_alpn_selected(ssl_, &proto, &proto_len);
    if (!proto || proto_len == 0) {
        return {};
    }
    return {reinterpret_cast<const char *>(proto), static_cast<std::size_t>(proto_len)};
}

bool TlsStreamFd::handshake_done() const noexcept { return handshake_done_; }

bool TlsStreamFd::has_pending_read() const noexcept { return ssl_ != nullptr && SSL_has_pending(ssl_) != 0; }

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
