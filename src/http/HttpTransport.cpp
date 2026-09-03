#include <fiber/http/HttpTransport.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <new>
#include <sys/uio.h>

#include <fiber/async/Timeout.h>
#include <fiber/common/Assert.h>
#include <fiber/net/IpAddress.h>

namespace fiber::http {

namespace {

constexpr int kMaxIov = 16;

// TLS writes go through SSL_write, which encrypts a contiguous input buffer into
// one record. Unlike sendmsg, BoringSSL has no scatter-gather API, so multi-node
// chains would otherwise produce one record per IoBuf. To collapse small adjacent
// nodes into a single record we coalesce them into a scratch buffer before the
// SSL_write call. Groups are kept <= kTlsCoalesceMax so each coalesced write fits
// in a single TLS record (default max fragment 16384). A group that is a single
// node -- including any oversized node -- is passed through to SSL_write with the
// node's own pointer (zero copy), so large bodies never touch the scratch buffer.
constexpr std::size_t kTlsCoalesceMax = 8192;

std::chrono::steady_clock::time_point make_deadline(std::chrono::milliseconds timeout) noexcept {
    if (timeout == std::chrono::milliseconds::max()) {
        return std::chrono::steady_clock::time_point::max();
    }
    return event::EventLoop::current().now() + timeout;
}

fiber::async::Task<common::IoResult<void>> invalid_tls_handshake() {
    co_return std::unexpected(common::IoErr::Invalid);
}

common::IoResult<std::chrono::milliseconds> remaining_timeout(std::chrono::steady_clock::time_point deadline) noexcept {
    if (deadline == std::chrono::steady_clock::time_point::max()) {
        return std::chrono::milliseconds::max();
    }
    auto now = event::EventLoop::current().now();
    if (deadline <= now) {
        return std::unexpected(common::IoErr::TimedOut);
    }
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    if (remaining <= std::chrono::milliseconds::zero()) {
        remaining = std::chrono::milliseconds(1);
    }
    return remaining;
}

fiber::async::Task<common::IoResult<void>> wait_tls_event(net::TlsTcpStream &stream, event::IoEvent io_event,
                                                          std::chrono::steady_clock::time_point deadline) {
    auto timeout_result = remaining_timeout(deadline);
    if (!timeout_result) {
        co_return std::unexpected(timeout_result.error());
    }
    common::IoResult<void> wait_result;
    if (io_event == event::IoEvent::Read) {
        wait_result = co_await fiber::async::timeout_for([&]() { return stream.wait_readable(); }, *timeout_result);
    } else if (io_event == event::IoEvent::Write) {
        wait_result = co_await fiber::async::timeout_for([&]() { return stream.wait_writable(); }, *timeout_result);
    } else {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!wait_result) {
        co_return std::unexpected(wait_result.error());
    }
    co_return common::IoResult<void>{};
}

} // namespace

common::IoResult<std::unique_ptr<TcpTransport>> TcpTransport::create(event::EventLoop &loop, net::AcceptResult &&accept,
                                                                     net::TcpSocketOptions tcp_options) {
    if (!accept.valid()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const common::IoErr option_err = net::detail::apply_tcp_socket_options(accept.fd(), tcp_options);
    if (option_err != common::IoErr::None) {
        return std::unexpected(option_err);
    }
    return std::unique_ptr<TcpTransport>(new TcpTransport(loop, accept.release_fd(), accept.take_peer()));
}

TcpTransport::TcpTransport(event::EventLoop &loop, int fd, net::SocketAddress remote_addr) :
    stream_(loop, fd, std::move(remote_addr)) {}

fiber::async::Task<common::IoResult<void>> TcpTransport::handshake(std::chrono::milliseconds) {
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<void>> TcpTransport::shutdown(std::chrono::milliseconds) {
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<void>> TcpTransport::wait_readable(std::chrono::milliseconds timeout) {
    auto result = co_await fiber::async::timeout_for([&]() { return stream_.wait_readable(); }, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    co_return common::IoResult<void>{};
}

common::IoErr TcpTransport::set_read_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_.set_read_callback(callback, ctx);
}

common::IoErr TcpTransport::set_write_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_.set_write_callback(callback, ctx);
}

common::IoErr TcpTransport::set_terminal_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_.set_terminal_callback(callback, ctx);
}

common::IoErr TcpTransport::clear_read_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_.clear_read_callback(callback, ctx);
}

common::IoErr TcpTransport::clear_write_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_.clear_write_callback(callback, ctx);
}

common::IoErr TcpTransport::clear_terminal_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_.clear_terminal_callback(callback, ctx);
}

common::IoErr TcpTransport::poll_read(void *buf, size_t len, size_t &out, event::IoEvent &wait_event) noexcept {
    out = 0;
    wait_event = event::IoEvent::None;
    auto result = stream_.try_read(buf, len);
    if (result) {
        out = *result;
        return common::IoErr::None;
    }
    if (result.error() == common::IoErr::WouldBlock) {
        wait_event = event::IoEvent::Read;
    }
    return result.error();
}

common::IoErr TcpTransport::poll_read_into(mem::IoBuf &buf, size_t &out, event::IoEvent &wait_event) noexcept {
    common::IoErr err = poll_read(buf.writable_data(), buf.writable(), out, wait_event);
    if (err == common::IoErr::None) {
        buf.commit(out);
    }
    return err;
}

common::IoErr TcpTransport::poll_readv_into(mem::IoBufChain &bufs, size_t &out, event::IoEvent &wait_event) noexcept {
    out = 0;
    wait_event = event::IoEvent::None;
    std::array<iovec, kMaxIov> iov{};
    int count = bufs.fill_read_iov(iov.data(), static_cast<int>(iov.size()));
    if (count == 0) {
        return common::IoErr::None;
    }
    auto result = stream_.try_readv(iov.data(), count);
    if (result) {
        out = *result;
        bufs.commit(out);
        return common::IoErr::None;
    }
    if (result.error() == common::IoErr::WouldBlock) {
        wait_event = event::IoEvent::Read;
    }
    return result.error();
}

common::IoErr TcpTransport::poll_write(const void *buf, size_t len, size_t &out, event::IoEvent &wait_event) noexcept {
    out = 0;
    wait_event = event::IoEvent::None;
    auto result = stream_.try_write(buf, len);
    if (result) {
        out = *result;
        return common::IoErr::None;
    }
    if (result.error() == common::IoErr::WouldBlock) {
        wait_event = event::IoEvent::Write;
    }
    return result.error();
}

common::IoErr TcpTransport::poll_write(mem::IoBuf &buf, size_t &out, event::IoEvent &wait_event) noexcept {
    common::IoErr err = poll_write(buf.readable_data(), buf.readable(), out, wait_event);
    if (err == common::IoErr::None) {
        buf.consume(out);
    }
    return err;
}

common::IoErr TcpTransport::poll_writev(mem::IoBufChain &buf, size_t &out, event::IoEvent &wait_event) noexcept {
    out = 0;
    wait_event = event::IoEvent::None;
    std::array<iovec, kMaxIov> iov{};
    int count = buf.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
    if (count == 0) {
        return common::IoErr::None;
    }
    auto result = stream_.try_writev(iov.data(), count);
    if (result) {
        out = *result;
        buf.consume_and_compact(out);
        return common::IoErr::None;
    }
    if (result.error() == common::IoErr::WouldBlock) {
        wait_event = event::IoEvent::Write;
    }
    return result.error();
}

fiber::async::Task<common::IoResult<size_t>> TcpTransport::read(void *buf, size_t len,
                                                                std::chrono::milliseconds timeout) {
    auto result = co_await stream_.read(buf, len, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    co_return *result;
}

fiber::async::Task<common::IoResult<size_t>> TcpTransport::read_into(mem::IoBuf &buf,
                                                                     std::chrono::milliseconds timeout) {
    size_t writable = buf.writable();
    if (writable == 0) {
        co_return static_cast<size_t>(0);
    }
    auto result = co_await stream_.read(buf.writable_data(), writable, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    buf.commit(*result);
    co_return *result;
}

fiber::async::Task<common::IoResult<size_t>> TcpTransport::readv_into(mem::IoBufChain &bufs,
                                                                      std::chrono::milliseconds timeout) {
    std::array<iovec, kMaxIov> iov{};
    int count = bufs.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
    if (count == 0) {
        co_return static_cast<size_t>(0);
    }
    auto result = co_await stream_.readv(iov.data(), count, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    bufs.commit(*result);
    co_return *result;
}

fiber::async::Task<common::IoResult<size_t>> TcpTransport::write(const void *buf, size_t len,
                                                                 std::chrono::milliseconds timeout) {
    auto result = co_await stream_.write(buf, len, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    co_return *result;
}

fiber::async::Task<common::IoResult<size_t>> TcpTransport::write(mem::IoBuf &buf, std::chrono::milliseconds timeout) {
    size_t readable = buf.readable();
    if (readable == 0) {
        co_return static_cast<size_t>(0);
    }
    auto result = co_await stream_.write(buf.readable_data(), readable, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    buf.consume(*result);
    co_return *result;
}

fiber::async::Task<common::IoResult<size_t>> TcpTransport::writev(mem::IoBufChain &buf,
                                                                  std::chrono::milliseconds timeout) {
    std::array<iovec, kMaxIov> iov{};
    int count = buf.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
    if (count == 0) {
        co_return static_cast<size_t>(0);
    }
    auto result = co_await stream_.writev(iov.data(), count, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    buf.consume_and_compact(*result);
    co_return *result;
}

void TcpTransport::close() { stream_.close(); }

bool TcpTransport::valid() const noexcept { return stream_.valid(); }

bool TcpTransport::terminal() const noexcept { return stream_.terminal(); }

int TcpTransport::fd() const noexcept { return stream_.fd(); }

std::string_view TcpTransport::negotiated_alpn() const noexcept { return {}; }

const net::SocketAddress &TcpTransport::remote_addr() const noexcept { return stream_.remote_addr(); }

event::EventLoop &TcpTransport::loop() const noexcept { return stream_.loop(); }

common::IoResult<std::unique_ptr<TlsTransport>> TlsTransport::create(event::EventLoop &loop, net::AcceptResult &&accept,
                                                                     net::TlsClientParam options,
                                                                     net::TcpSocketOptions tcp_options) {
    if (!accept.valid()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const common::IoErr option_err = net::detail::apply_tcp_socket_options(accept.fd(), tcp_options);
    if (option_err != common::IoErr::None) {
        return std::unexpected(option_err);
    }
    auto transport = std::unique_ptr<TlsTransport>(
            new TlsTransport(loop, accept.release_fd(), accept.take_peer(), std::move(options)));
    return transport;
}

common::IoResult<std::unique_ptr<TlsTransport>> TlsTransport::create(event::EventLoop &loop, net::AcceptResult &&accept,
                                                                     const net::TlsServerParam &options,
                                                                     net::TcpSocketOptions tcp_options) {
    if (!accept.valid()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const common::IoErr option_err = net::detail::apply_tcp_socket_options(accept.fd(), tcp_options);
    if (option_err != common::IoErr::None) {
        return std::unexpected(option_err);
    }
    auto transport =
            std::unique_ptr<TlsTransport>(new TlsTransport(loop, accept.release_fd(), accept.take_peer(), options));
    return transport;
}

TlsTransport::TlsTransport(event::EventLoop &loop, int fd, net::SocketAddress remote_addr,
                           net::TlsClientParam options) :
    stream_(loop, fd, std::move(remote_addr)), client_options_(std::move(options)) {}

TlsTransport::TlsTransport(event::EventLoop &loop, int fd, net::SocketAddress remote_addr,
                           const net::TlsServerParam &options) :
    stream_(loop, fd, std::move(remote_addr)), server_options_(&options) {}

TlsTransport::~TlsTransport() = default;

fiber::async::Task<common::IoResult<void>> TlsTransport::wait_readable(std::chrono::milliseconds timeout) {
    FIBER_ASSERT(handshake_done());
    if (stream_.has_pending_read()) {
        co_return common::IoResult<void>{};
    }

    auto result = co_await fiber::async::timeout_for([&]() { return stream_.wait_readable(); }, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    co_return common::IoResult<void>{};
}

common::IoErr TlsTransport::set_read_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_.set_read_callback(callback, ctx);
}

common::IoErr TlsTransport::set_write_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_.set_write_callback(callback, ctx);
}

common::IoErr TlsTransport::set_terminal_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_.set_terminal_callback(callback, ctx);
}

common::IoErr TlsTransport::clear_read_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_.clear_read_callback(callback, ctx);
}

common::IoErr TlsTransport::clear_write_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_.clear_write_callback(callback, ctx);
}

common::IoErr TlsTransport::clear_terminal_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_.clear_terminal_callback(callback, ctx);
}

common::IoErr TlsTransport::poll_read(void *buf, size_t len, size_t &out, event::IoEvent &wait_event) noexcept {
    out = 0;
    wait_event = event::IoEvent::None;
    FIBER_ASSERT(handshake_done());
    if (len == 0) {
        return common::IoErr::None;
    }
    return stream_.poll_read(buf, len, out, wait_event);
}

common::IoErr TlsTransport::poll_read_into(mem::IoBuf &buf, size_t &out, event::IoEvent &wait_event) noexcept {
    common::IoErr err = poll_read(buf.writable_data(), buf.writable(), out, wait_event);
    if (err == common::IoErr::None) {
        buf.commit(out);
    }
    return err;
}

common::IoErr TlsTransport::poll_readv_into(mem::IoBufChain &bufs, size_t &out, event::IoEvent &wait_event) noexcept {
    mem::IoBuf *target = bufs.first_writable();
    if (!target) {
        out = 0;
        wait_event = event::IoEvent::None;
        return common::IoErr::None;
    }
    return poll_read_into(*target, out, wait_event);
}

common::IoErr TlsTransport::poll_write(const void *buf, size_t len, size_t &out, event::IoEvent &wait_event) noexcept {
    out = 0;
    wait_event = event::IoEvent::None;
    FIBER_ASSERT(handshake_done());
    if (len == 0) {
        return common::IoErr::None;
    }
    if (pending_write_kind_ == PendingWriteKind::Chain) {
        return common::IoErr::Busy;
    }
    if (pending_write_kind_ == PendingWriteKind::Contiguous &&
        (pending_write_data_ != buf || pending_write_len_ != len)) {
        return common::IoErr::Busy;
    }

    common::IoErr err = stream_.poll_write(buf, len, out, wait_event);
    if (err == common::IoErr::WouldBlock) {
        pending_write_kind_ = PendingWriteKind::Contiguous;
        pending_write_data_ = buf;
        pending_write_len_ = len;
        return err;
    }
    clear_pending_write();
    return err;
}

common::IoErr TlsTransport::poll_write(mem::IoBuf &buf, size_t &out, event::IoEvent &wait_event) noexcept {
    common::IoErr err = poll_write(buf.readable_data(), buf.readable(), out, wait_event);
    if (err == common::IoErr::None) {
        buf.consume(out);
    }
    return err;
}

common::IoErr TlsTransport::poll_writev(mem::IoBufChain &buf, size_t &out, event::IoEvent &wait_event) noexcept {
    out = 0;
    wait_event = event::IoEvent::None;
    FIBER_ASSERT(handshake_done());

    if (pending_write_kind_ == PendingWriteKind::Contiguous) {
        return common::IoErr::Busy;
    }
    if (pending_write_kind_ == PendingWriteKind::Chain && pending_write_chain_ != &buf) {
        return common::IoErr::Busy;
    }

    const void *write_data = pending_write_data_;
    std::size_t write_len = pending_write_len_;
    if (pending_write_kind_ == PendingWriteKind::None) {
        std::array<iovec, kMaxIov> iov{};
        int count = buf.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
        if (count == 0) {
            return common::IoErr::None;
        }

        int group_end = 0;
        std::size_t group_len = 0;
        while (group_end < count && group_len + iov[group_end].iov_len <= kTlsCoalesceMax) {
            group_len += iov[group_end].iov_len;
            ++group_end;
        }
        if (group_end == 0) {
            group_end = 1;
            group_len = iov[0].iov_len;
        }

        if (group_end == 1) {
            write_data = iov[0].iov_base;
        } else {
            if (!writev_scratch_) {
                writev_scratch_.reset(new (std::nothrow) std::uint8_t[kTlsCoalesceMax]);
                if (!writev_scratch_) {
                    return common::IoErr::NoMem;
                }
            }
            std::uint8_t *dst = writev_scratch_.get();
            for (int i = 0; i < group_end; ++i) {
                std::memcpy(dst, iov[i].iov_base, iov[i].iov_len);
                dst += iov[i].iov_len;
            }
            write_data = writev_scratch_.get();
        }
        write_len = group_len;
    }

    common::IoErr err = stream_.poll_write(write_data, write_len, out, wait_event);
    if (err == common::IoErr::WouldBlock) {
        pending_write_kind_ = PendingWriteKind::Chain;
        pending_write_data_ = write_data;
        pending_write_len_ = write_len;
        pending_write_chain_ = &buf;
        return err;
    }
    clear_pending_write();
    if (err == common::IoErr::None) {
        buf.consume_and_compact(out);
    }
    return err;
}

bool TlsTransport::handshake_done() const noexcept { return stream_.handshake_done(); }

void TlsTransport::clear_pending_write() noexcept {
    pending_write_kind_ = PendingWriteKind::None;
    pending_write_data_ = nullptr;
    pending_write_len_ = 0;
    pending_write_chain_ = nullptr;
}

fiber::async::Task<common::IoResult<void>> TlsTransport::handshake(std::chrono::milliseconds timeout) {
    clear_pending_write();
    if (server_options_) {
        return stream_.handshake(*server_options_, timeout);
    }
    if (client_options_) {
        return stream_.handshake(*client_options_, timeout);
    }
    FIBER_ASSERT(false);
    return invalid_tls_handshake();
}

fiber::async::Task<common::IoResult<void>> TlsTransport::shutdown(std::chrono::milliseconds timeout) {
    auto deadline = make_deadline(timeout);
    for (;;) {
        event::IoEvent wait_event = event::IoEvent::None;
        common::IoErr err = stream_.poll_shutdown(wait_event);
        if (err == common::IoErr::None) {
            co_return common::IoResult<void>{};
        }
        if (err != common::IoErr::WouldBlock) {
            co_return std::unexpected(err);
        }
        auto wait_result = co_await wait_tls_event(stream_, wait_event, deadline);
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
}

fiber::async::Task<common::IoResult<size_t>> TlsTransport::read(void *buf, size_t len,
                                                                std::chrono::milliseconds timeout) {
    FIBER_ASSERT(handshake_done());
    auto deadline = make_deadline(timeout);
    for (;;) {
        size_t out = 0;
        event::IoEvent wait_event = event::IoEvent::None;
        common::IoErr err = poll_read(buf, len, out, wait_event);
        if (err == common::IoErr::None) {
            co_return out;
        }
        if (err != common::IoErr::WouldBlock) {
            co_return std::unexpected(err);
        }
        auto wait_result = co_await wait_tls_event(stream_, wait_event, deadline);
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
}

fiber::async::Task<common::IoResult<size_t>> TlsTransport::read_into(mem::IoBuf &buf,
                                                                     std::chrono::milliseconds timeout) {
    size_t writable = buf.writable();
    if (writable == 0) {
        co_return static_cast<size_t>(0);
    }
    FIBER_ASSERT(handshake_done());
    auto deadline = make_deadline(timeout);
    for (;;) {
        size_t out = 0;
        event::IoEvent wait_event = event::IoEvent::None;
        common::IoErr err = poll_read_into(buf, out, wait_event);
        if (err == common::IoErr::None) {
            co_return out;
        }
        if (err != common::IoErr::WouldBlock) {
            co_return std::unexpected(err);
        }
        auto wait_result = co_await wait_tls_event(stream_, wait_event, deadline);
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
}

fiber::async::Task<common::IoResult<size_t>> TlsTransport::readv_into(mem::IoBufChain &bufs,
                                                                      std::chrono::milliseconds timeout) {
    mem::IoBuf *target = bufs.first_writable();
    if (!target) {
        co_return static_cast<size_t>(0);
    }
    co_return co_await read_into(*target, timeout);
}

fiber::async::Task<common::IoResult<size_t>> TlsTransport::write(const void *buf, size_t len,
                                                                 std::chrono::milliseconds timeout) {
    FIBER_ASSERT(handshake_done());
    auto deadline = make_deadline(timeout);
    for (;;) {
        size_t out = 0;
        event::IoEvent wait_event = event::IoEvent::None;
        common::IoErr err = poll_write(buf, len, out, wait_event);
        if (err == common::IoErr::None) {
            co_return out;
        }
        if (err != common::IoErr::WouldBlock) {
            co_return std::unexpected(err);
        }
        auto wait_result = co_await wait_tls_event(stream_, wait_event, deadline);
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
}

fiber::async::Task<common::IoResult<size_t>> TlsTransport::write(mem::IoBuf &buf, std::chrono::milliseconds timeout) {
    size_t readable = buf.readable();
    if (readable == 0) {
        co_return static_cast<size_t>(0);
    }
    FIBER_ASSERT(handshake_done());
    auto deadline = make_deadline(timeout);
    for (;;) {
        size_t out = 0;
        event::IoEvent wait_event = event::IoEvent::None;
        common::IoErr err = poll_write(buf, out, wait_event);
        if (err == common::IoErr::None) {
            co_return out;
        }
        if (err != common::IoErr::WouldBlock) {
            co_return std::unexpected(err);
        }
        auto wait_result = co_await wait_tls_event(stream_, wait_event, deadline);
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
}

fiber::async::Task<common::IoResult<size_t>> TlsTransport::writev(mem::IoBufChain &buf,
                                                                  std::chrono::milliseconds timeout) {
    FIBER_ASSERT(handshake_done());
    auto deadline = make_deadline(timeout);

    for (;;) {
        if (buf.readable_bytes() == 0) {
            co_return static_cast<std::size_t>(0);
        }

        std::size_t out = 0;
        event::IoEvent wait_event = event::IoEvent::None;
        common::IoErr err = poll_writev(buf, out, wait_event);
        if (err == common::IoErr::None) {
            co_return out;
        }
        if (err != common::IoErr::WouldBlock) {
            co_return std::unexpected(err);
        }

        auto wait_result = co_await wait_tls_event(stream_, wait_event, deadline);
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
}

void TlsTransport::abandon_pending_io() noexcept { clear_pending_write(); }

void TlsTransport::close() {
    clear_pending_write();
    writev_scratch_.reset();
    stream_.close();
}

bool TlsTransport::valid() const noexcept { return stream_.valid(); }

bool TlsTransport::terminal() const noexcept { return stream_.terminal(); }

bool TlsTransport::has_pending_read() const noexcept { return stream_.has_pending_read(); }

int TlsTransport::fd() const noexcept { return stream_.fd(); }

std::string_view TlsTransport::negotiated_alpn() const noexcept { return stream_.selected_alpn(); }

const net::SocketAddress &TlsTransport::remote_addr() const noexcept { return stream_.remote_addr(); }

event::EventLoop &TlsTransport::loop() const noexcept { return stream_.loop(); }

} // namespace fiber::http
