#include <fiber/net/detail/TlsStreamFd.h>

#include <cerrno>
#include <cstdint>
#include <sys/socket.h>

#include <fiber/common/Assert.h>

#include <openssl/bio.h>
#include <openssl/ssl.h>

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

common::IoResult<void> TlsStreamFd::init(SSL *ssl) {
    if (!ssl) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (!stream_fd_.valid()) {
        SSL_free(ssl);
        return std::unexpected(common::IoErr::BadFd);
    }
    if (ssl_) {
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    ssl_ = ssl;
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

fiber::common::IoErr TlsStreamFd::set_read_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_fd_.set_read_callback(callback, ctx);
}

fiber::common::IoErr TlsStreamFd::set_write_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_fd_.set_write_callback(callback, ctx);
}

fiber::common::IoErr TlsStreamFd::set_terminal_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_fd_.set_terminal_callback(callback, ctx);
}

fiber::common::IoErr TlsStreamFd::clear_read_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_fd_.clear_read_callback(callback, ctx);
}

fiber::common::IoErr TlsStreamFd::clear_write_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_fd_.clear_write_callback(callback, ctx);
}

fiber::common::IoErr TlsStreamFd::clear_terminal_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_fd_.clear_terminal_callback(callback, ctx);
}

TlsStreamFd::IoTask TlsStreamFd::read(void *buf, size_t len, std::chrono::milliseconds timeout) noexcept {
    if (busy_) {
        co_return std::unexpected(fiber::common::IoErr::Busy);
    }

    busy_ = true;
    BusyResetGuard busy_reset(&busy_);
    Deadline deadline = make_deadline(timeout);
    for (;;) {
        size_t out = 0;
        fiber::event::IoEvent wait_event = fiber::event::IoEvent::None;
        fiber::common::IoErr err = read_once(buf, len, out, wait_event);
        if (err == fiber::common::IoErr::None) {
            co_return out;
        }
        if (err != fiber::common::IoErr::WouldBlock) {
            co_return std::unexpected(err);
        }
        auto remaining = remaining_timeout(deadline);
        if (!remaining) {
            co_return std::unexpected(remaining.error());
        }
        fiber::common::IoResult<void> wait_result;
        if (wait_event == fiber::event::IoEvent::Read) {
            wait_result = co_await stream_fd_.wait_readable(*remaining);
        } else if (wait_event == fiber::event::IoEvent::Write) {
            wait_result = co_await stream_fd_.wait_writable(*remaining);
        } else {
            co_return std::unexpected(fiber::common::IoErr::Invalid);
        }
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
}

TlsStreamFd::IoTask TlsStreamFd::write(const void *buf, size_t len, std::chrono::milliseconds timeout) noexcept {
    if (busy_) {
        co_return std::unexpected(fiber::common::IoErr::Busy);
    }

    busy_ = true;
    BusyResetGuard busy_reset(&busy_);
    Deadline deadline = make_deadline(timeout);
    for (;;) {
        size_t out = 0;
        fiber::event::IoEvent wait_event = fiber::event::IoEvent::None;
        fiber::common::IoErr err = write_once(buf, len, out, wait_event);
        if (err == fiber::common::IoErr::None) {
            co_return out;
        }
        if (err != fiber::common::IoErr::WouldBlock) {
            co_return std::unexpected(err);
        }
        auto remaining = remaining_timeout(deadline);
        if (!remaining) {
            co_return std::unexpected(remaining.error());
        }
        fiber::common::IoResult<void> wait_result;
        if (wait_event == fiber::event::IoEvent::Read) {
            wait_result = co_await stream_fd_.wait_readable(*remaining);
        } else if (wait_event == fiber::event::IoEvent::Write) {
            wait_result = co_await stream_fd_.wait_writable(*remaining);
        } else {
            co_return std::unexpected(fiber::common::IoErr::Invalid);
        }
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
}

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

StreamFd::WaitReadableAwaiter TlsStreamFd::wait_readable(std::chrono::milliseconds timeout) noexcept {
    return stream_fd_.wait_readable(timeout);
}

StreamFd::WaitWritableAwaiter TlsStreamFd::wait_writable(std::chrono::milliseconds timeout) noexcept {
    return stream_fd_.wait_writable(timeout);
}

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

} // namespace fiber::net::detail
