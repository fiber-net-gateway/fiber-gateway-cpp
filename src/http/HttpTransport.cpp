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
// chains would otherwise produce one record per IoBuf: an HTTP/2 DATA frame is
// a 9-byte header node followed by its payload node, and writing the header as
// its own record costs a full send() for 31 bytes on the wire. A node that is
// itself at least one full record is passed to SSL_write with the node's own
// pointer (zero copy). Anything smaller is coalesced into a scratch buffer with
// the nodes that follow it, splitting the last node when needed, so that each
// scratch write is a full record whenever the chain holds enough data.
constexpr std::size_t kTlsRecordPlaintextMax = 16384;

std::chrono::steady_clock::time_point make_deadline(std::chrono::milliseconds timeout) noexcept {
    if (timeout == std::chrono::milliseconds::max()) {
        return std::chrono::steady_clock::time_point::max();
    }
    return event::EventLoop::current().now() + timeout;
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

bool TcpTransport::read_ready() const noexcept { return stream_.read_ready(); }

bool TcpTransport::write_ready() const noexcept { return stream_.write_ready(); }

common::IoErr TcpTransport::detach_for_handover() noexcept { return stream_.detach_for_handover(); }

common::IoErr TcpTransport::adopt_loop(event::EventLoop &loop) noexcept { return stream_.adopt_loop(loop); }

common::IoErr TcpTransport::ensure_state_observation() noexcept { return stream_.ensure_state_observation(); }

common::IoResult<size_t> TcpTransport::try_readv(size_t size, mem::IoBufChain &out) noexcept {
    if (size == 0) {
        return static_cast<size_t>(0);
    }
    mem::IoBuf node = mem::IoBuf::allocate(size);
    if (!node) {
        return std::unexpected(common::IoErr::NoMem);
    }
    auto result = stream_.try_read(node.writable_data(), node.writable());
    if (!result) {
        return std::unexpected(result.error());
    }
    if (*result == 0) {
        return static_cast<size_t>(0);
    }
    node.commit(*result);
    if (!out.append(std::move(node))) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return *result;
}

fiber::async::Task<common::IoResult<size_t>> TcpTransport::readv(size_t size, mem::IoBufChain &out,
                                                                 std::chrono::milliseconds timeout) {
    auto deadline = make_deadline(timeout);
    for (;;) {
        auto result = try_readv(size, out);
        if (result || result.error() != common::IoErr::WouldBlock) {
            co_return result;
        }
        auto timeout_result = remaining_timeout(deadline);
        if (!timeout_result) {
            co_return std::unexpected(timeout_result.error());
        }
        auto wait_result =
                co_await fiber::async::timeout_for([&]() { return stream_.wait_readable(); }, *timeout_result);
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
}

common::IoResult<size_t> TcpTransport::try_writev(mem::IoBufChain &buf) noexcept {
    std::array<iovec, kMaxIov> iov{};
    int count = buf.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
    if (count == 0) {
        return static_cast<size_t>(0);
    }
    auto result = stream_.try_writev(iov.data(), count);
    if (!result) {
        return std::unexpected(result.error());
    }
    buf.consume_and_compact(*result);
    return *result;
}

fiber::async::Task<common::IoResult<size_t>> TcpTransport::writev(mem::IoBufChain &buf,
                                                                  std::chrono::milliseconds timeout) {
    auto deadline = make_deadline(timeout);
    for (;;) {
        if (buf.readable_bytes() == 0) {
            co_return static_cast<size_t>(0);
        }
        auto result = try_writev(buf);
        if (result || result.error() != common::IoErr::WouldBlock) {
            co_return result;
        }
        auto timeout_result = remaining_timeout(deadline);
        if (!timeout_result) {
            co_return std::unexpected(timeout_result.error());
        }
        auto wait_result =
                co_await fiber::async::timeout_for([&]() { return stream_.wait_writable(); }, *timeout_result);
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
}

void TcpTransport::close() { stream_.close(); }

bool TcpTransport::valid() const noexcept { return stream_.valid(); }

bool TcpTransport::terminal() const noexcept { return stream_.terminal(); }
bool TcpTransport::peer_closed() const noexcept { return stream_.peer_closed(); }

int TcpTransport::fd() const noexcept { return stream_.fd(); }

std::string_view TcpTransport::negotiated_alpn() const noexcept { return {}; }

const net::SocketAddress &TcpTransport::remote_addr() const noexcept { return stream_.remote_addr(); }

event::EventLoop &TcpTransport::loop() const noexcept { return stream_.loop(); }

common::IoResult<std::unique_ptr<TlsTransport>> TlsTransport::create(event::EventLoop &loop, net::AcceptResult &&accept,
                                                                     net::TcpSocketOptions tcp_options) {
    if (!accept.valid()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const common::IoErr option_err = net::detail::apply_tcp_socket_options(accept.fd(), tcp_options);
    if (option_err != common::IoErr::None) {
        return std::unexpected(option_err);
    }
    auto transport = std::unique_ptr<TlsTransport>(new TlsTransport(loop, accept.release_fd(), accept.take_peer()));
    return transport;
}

TlsTransport::TlsTransport(event::EventLoop &loop, int fd, net::SocketAddress remote_addr) :
    stream_(loop, fd, std::move(remote_addr)) {}

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

// Pending decrypted plaintext behaves like a ready fd: a poll_read makes
// progress without waiting, so callers must advance it instead of subscribing.
bool TlsTransport::read_ready() const noexcept { return stream_.has_pending_read() || stream_.read_ready(); }

bool TlsTransport::write_ready() const noexcept { return stream_.write_ready(); }

common::IoErr TlsTransport::detach_for_handover() noexcept {
    FIBER_ASSERT(pending_write_kind_ == PendingWriteKind::None);
    return stream_.detach_for_handover();
}

common::IoErr TlsTransport::adopt_loop(event::EventLoop &loop) noexcept { return stream_.adopt_loop(loop); }

common::IoErr TlsTransport::ensure_state_observation() noexcept { return stream_.ensure_state_observation(); }

common::IoResult<size_t> TlsTransport::try_readv(size_t size, mem::IoBufChain &out) noexcept {
    std::size_t out_bytes = 0;
    event::IoEvent wait_event = event::IoEvent::None;
    common::IoErr err = poll_read_node(size, out, out_bytes, wait_event);
    if (err != common::IoErr::None) {
        return std::unexpected(err);
    }
    return out_bytes;
}

common::IoErr TlsTransport::poll_read_node(std::size_t size, mem::IoBufChain &out, std::size_t &out_bytes,
                                           event::IoEvent &wait_event) noexcept {
    out_bytes = 0;
    wait_event = event::IoEvent::None;
    FIBER_ASSERT(handshake_done());
    if (size == 0) {
        return common::IoErr::None;
    }
    // SSL_read delivers at most one record's plaintext per call, so a larger
    // node would only hold unwritten capacity.
    size = std::min(size, kTlsRecordPlaintextMax);
    mem::IoBuf node = mem::IoBuf::allocate(size);
    if (!node) {
        return common::IoErr::NoMem;
    }
    std::size_t got = 0;
    common::IoErr err = stream_.poll_read(node.writable_data(), node.writable(), got, wait_event);
    if (err != common::IoErr::None) {
        return err;
    }
    if (got == 0) {
        return common::IoErr::None; // EOF (close_notify)
    }
    node.commit(got);
    if (!out.append(std::move(node))) {
        return common::IoErr::NoMem;
    }
    out_bytes = got;
    return common::IoErr::None;
}

common::IoResult<size_t> TlsTransport::try_writev(mem::IoBufChain &buf) noexcept {
    std::size_t out = 0;
    event::IoEvent wait_event = event::IoEvent::None;
    common::IoErr err = poll_write_chain(buf, out, wait_event);
    if (err != common::IoErr::None) {
        return std::unexpected(err);
    }
    return out;
}

common::IoErr TlsTransport::poll_write_chain(mem::IoBufChain &buf, std::size_t &out,
                                             event::IoEvent &wait_event) noexcept {
    out = 0;
    wait_event = event::IoEvent::None;
    FIBER_ASSERT(handshake_done());

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

        if (count == 1) {
            write_data = iov[0].iov_base;
            write_len = iov[0].iov_len;
        } else if (iov[0].iov_len >= kTlsRecordPlaintextMax) {
            // Whole records straight from the node; its tail joins the next
            // group so it does not become a short record of its own.
            write_data = iov[0].iov_base;
            write_len = iov[0].iov_len - iov[0].iov_len % kTlsRecordPlaintextMax;
        } else {
            if (!writev_scratch_) {
                writev_scratch_.reset(new (std::nothrow) std::uint8_t[kTlsRecordPlaintextMax]);
                if (!writev_scratch_) {
                    return common::IoErr::NoMem;
                }
            }
            std::uint8_t *dst = writev_scratch_.get();
            std::size_t group_len = 0;
            for (int i = 0; i < count && group_len < kTlsRecordPlaintextMax; ++i) {
                const std::size_t take = std::min(iov[i].iov_len, kTlsRecordPlaintextMax - group_len);
                std::memcpy(dst + group_len, iov[i].iov_base, take);
                group_len += take;
            }
            write_data = writev_scratch_.get();
            write_len = group_len;
        }
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

fiber::async::Task<common::IoResult<void>> TlsTransport::handshake(const net::TlsClientParam &param,
                                                                   std::chrono::milliseconds timeout) {
    clear_pending_write();
    return stream_.handshake(param, timeout);
}

fiber::async::Task<common::IoResult<void>> TlsTransport::handshake(const net::TlsServerParam &param,
                                                                   std::chrono::milliseconds timeout) {
    clear_pending_write();
    return stream_.handshake(param, timeout);
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

fiber::async::Task<common::IoResult<size_t>> TlsTransport::readv(size_t size, mem::IoBufChain &out,
                                                                 std::chrono::milliseconds timeout) {
    FIBER_ASSERT(handshake_done());
    auto deadline = make_deadline(timeout);
    for (;;) {
        std::size_t out_bytes = 0;
        event::IoEvent wait_event = event::IoEvent::None;
        common::IoErr err = poll_read_node(size, out, out_bytes, wait_event);
        if (err == common::IoErr::None) {
            co_return out_bytes;
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
        common::IoErr err = poll_write_chain(buf, out, wait_event);
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
bool TlsTransport::peer_closed() const noexcept { return stream_.peer_closed(); }

bool TlsTransport::has_pending_read() const noexcept { return stream_.has_pending_read(); }

int TlsTransport::fd() const noexcept { return stream_.fd(); }

std::string_view TlsTransport::negotiated_alpn() const noexcept { return stream_.selected_alpn(); }

const net::SocketAddress &TlsTransport::remote_addr() const noexcept { return stream_.remote_addr(); }

event::EventLoop &TlsTransport::loop() const noexcept { return stream_.loop(); }

} // namespace fiber::http
