#include "HttpTransport.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <sys/uio.h>

#include <openssl/ssl.h>
#include <openssl/tls1.h>

#include "../async/Timeout.h"
#include "../common/Assert.h"

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

common::IoResult<std::chrono::milliseconds> remaining_timeout(event::EventLoop &loop,
                                                              std::chrono::steady_clock::time_point deadline) {
    if (deadline == std::chrono::steady_clock::time_point::max()) {
        return std::chrono::milliseconds::max();
    }
    auto now = loop.now();
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
    auto timeout_result = remaining_timeout(stream.loop(), deadline);
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

common::IoResult<std::unique_ptr<TcpTransport>> TcpTransport::create(event::EventLoop &loop,
                                                                     net::AcceptResult &&accept) {
    if (!accept.valid()) {
        return std::unexpected(common::IoErr::Invalid);
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

fiber::async::Task<common::IoResult<size_t>> TcpTransport::read(void *buf, size_t len,
                                                                std::chrono::milliseconds timeout) {
    auto result = co_await fiber::async::timeout_for([&]() { return stream_.read(buf, len); }, timeout);
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
    auto result =
            co_await fiber::async::timeout_for([&]() { return stream_.read(buf.writable_data(), writable); }, timeout);
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
    auto result = co_await fiber::async::timeout_for([&]() { return stream_.readv(iov.data(), count); }, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    bufs.commit(*result);
    co_return *result;
}

fiber::async::Task<common::IoResult<size_t>> TcpTransport::write(const void *buf, size_t len,
                                                                 std::chrono::milliseconds timeout) {
    auto result = co_await fiber::async::timeout_for([&]() { return stream_.write(buf, len); }, timeout);
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
    auto result =
            co_await fiber::async::timeout_for([&]() { return stream_.write(buf.readable_data(), readable); }, timeout);
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
    auto result = co_await fiber::async::timeout_for([&]() { return stream_.writev(iov.data(), count); }, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    buf.consume_and_compact(*result);
    co_return *result;
}

void TcpTransport::close() { stream_.close(); }

bool TcpTransport::valid() const noexcept { return stream_.valid(); }

int TcpTransport::fd() const noexcept { return stream_.fd(); }

std::string_view TcpTransport::negotiated_alpn() const noexcept { return {}; }

const net::SocketAddress &TcpTransport::remote_addr() const noexcept { return stream_.remote_addr(); }

event::EventLoop &TcpTransport::loop() const noexcept { return stream_.loop(); }

common::IoResult<std::unique_ptr<TlsTransport>> TlsTransport::create(event::EventLoop &loop, net::AcceptResult &&accept,
                                                                     net::TlsContext &context) {
    if (!accept.valid()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    auto transport =
            std::unique_ptr<TlsTransport>(new TlsTransport(loop, accept.release_fd(), accept.take_peer(), context));
    auto init_result = transport->init();
    if (!init_result) {
        return std::unexpected(init_result.error());
    }
    return transport;
}

common::IoResult<std::unique_ptr<TlsTransport>> TlsTransport::create(event::EventLoop &loop, net::AcceptResult &&accept,
                                                                     net::TlsServerContext &context) {
    if (!accept.valid()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    auto transport =
            std::unique_ptr<TlsTransport>(new TlsTransport(loop, accept.release_fd(), accept.take_peer(), context));
    auto init_result = transport->init();
    if (!init_result) {
        return std::unexpected(init_result.error());
    }
    return transport;
}

TlsTransport::TlsTransport(event::EventLoop &loop, int fd, net::SocketAddress remote_addr, net::TlsContext &context) :
    stream_(loop, fd, std::move(remote_addr)), context_(&context) {}

TlsTransport::TlsTransport(event::EventLoop &loop, int fd, net::SocketAddress remote_addr,
                           net::TlsServerContext &context) :
    stream_(loop, fd, std::move(remote_addr)), server_context_(&context) {}

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

common::IoResult<void> TlsTransport::init() {
    SSL_CTX *ctx = nullptr;
    bool is_server = false;
    if (server_context_) {
        ctx = server_context_->raw();
        is_server = true;
    } else if (context_) {
        ctx = context_->raw();
        is_server = context_->is_server();
    }
    if (!ctx) {
        return std::unexpected(common::IoErr::Invalid);
    }
    auto init_result = stream_.init(ctx, is_server, &TlsTransport::configure_ssl, this);
    if (!init_result) {
        return std::unexpected(init_result.error());
    }
    return {};
}

void TlsTransport::configure_ssl(SSL *ssl, void *ctx) noexcept {
    auto *self = static_cast<TlsTransport *>(ctx);
    if (!self || !ssl) {
        return;
    }
    if (self->server_context_) {
        (void) self->server_context_->bind_ssl(ssl, &self->stream_.remote_addr());
        return;
    }
    if (!self->context_ || self->context_->is_server()) {
        return;
    }
    if (!self->context_->options().server_name.empty()) {
        (void) SSL_set_tlsext_host_name(ssl, self->context_->options().server_name.c_str());
    }
}

bool TlsTransport::handshake_done() const noexcept { return stream_.handshake_done(); }

fiber::async::Task<common::IoResult<void>> TlsTransport::handshake(std::chrono::milliseconds timeout) {
    auto deadline = (timeout == std::chrono::milliseconds::max()) ? std::chrono::steady_clock::time_point::max()
                                                                  : stream_.loop().now() + timeout;
    for (;;) {
        event::IoEvent wait_event = event::IoEvent::None;
        common::IoErr err = stream_.poll_handshake(wait_event);
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

fiber::async::Task<common::IoResult<void>> TlsTransport::shutdown(std::chrono::milliseconds timeout) {
    auto deadline = (timeout == std::chrono::milliseconds::max()) ? std::chrono::steady_clock::time_point::max()
                                                                  : stream_.loop().now() + timeout;
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
    auto deadline = (timeout == std::chrono::milliseconds::max()) ? std::chrono::steady_clock::time_point::max()
                                                                  : stream_.loop().now() + timeout;
    for (;;) {
        size_t out = 0;
        event::IoEvent wait_event = event::IoEvent::None;
        common::IoErr err = stream_.poll_read(buf, len, out, wait_event);
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
    auto deadline = (timeout == std::chrono::milliseconds::max()) ? std::chrono::steady_clock::time_point::max()
                                                                  : stream_.loop().now() + timeout;
    for (;;) {
        size_t out = 0;
        event::IoEvent wait_event = event::IoEvent::None;
        common::IoErr err = stream_.poll_read(buf.writable_data(), writable, out, wait_event);
        if (err == common::IoErr::None) {
            buf.commit(out);
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
    auto deadline = (timeout == std::chrono::milliseconds::max()) ? std::chrono::steady_clock::time_point::max()
                                                                  : stream_.loop().now() + timeout;
    for (;;) {
        size_t out = 0;
        event::IoEvent wait_event = event::IoEvent::None;
        common::IoErr err = stream_.poll_write(buf, len, out, wait_event);
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
    auto deadline = (timeout == std::chrono::milliseconds::max()) ? std::chrono::steady_clock::time_point::max()
                                                                  : stream_.loop().now() + timeout;
    for (;;) {
        size_t out = 0;
        event::IoEvent wait_event = event::IoEvent::None;
        common::IoErr err = stream_.poll_write(buf.readable_data(), readable, out, wait_event);
        if (err == common::IoErr::None) {
            buf.consume(out);
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

    auto deadline = (timeout == std::chrono::milliseconds::max()) ? std::chrono::steady_clock::time_point::max()
                                                                  : stream_.loop().now() + timeout;
    std::size_t total_written = 0;

    // Per-call coalesce scratch. Lazily allocated the first time a multi-node group
    // is formed, then reused for every subsequent group in this writev call and freed
    // when the coroutine frame is destroyed. Lives across WouldBlock suspensions
    // (coroutine frame); safe from concurrent writers because TlsStreamFd serializes
    // read/write via busy_. Never used by single-node groups (including oversized
    // nodes), so large bodies stay zero-copy.
    std::unique_ptr<std::uint8_t[]> scratch;
    std::array<iovec, kMaxIov> iov{};

    for (;;) {
        buf.drop_empty_front();
        if (buf.readable_bytes() == 0) {
            co_return total_written;
        }
        int count = buf.fill_write_iov(iov.data(), static_cast<int>(iov.size()));

        // Greedily accumulate a group of adjacent nodes whose total fits in one
        // coalesced record. Nodes are taken from the front of the snapshot; a node
        // that would overflow the budget flushes the current group first.
        int group_end = 0;
        std::size_t group_len = 0;
        while (group_end < count && group_len + iov[group_end].iov_len <= kTlsCoalesceMax) {
            group_len += iov[group_end].iov_len;
            ++group_end;
        }
        if (group_end == 0) {
            // First node alone exceeds the budget: write it solo (zero copy). This
            // also guarantees forward progress for oversized nodes.
            group_end = 1;
            group_len = iov[0].iov_len;
        }

        const std::uint8_t *write_buf = nullptr;
        if (group_end == 1) {
            // Single node: pass its own pointer straight to SSL_write. No copy.
            write_buf = static_cast<const std::uint8_t *>(iov[0].iov_base);
        } else {
            if (!scratch) {
                scratch = std::make_unique<std::uint8_t[]>(kTlsCoalesceMax);
            }
            std::uint8_t *dst = scratch.get();
            for (int i = 0; i < group_end; ++i) {
                std::memcpy(dst, iov[i].iov_base, iov[i].iov_len);
                dst += iov[i].iov_len;
            }
            write_buf = scratch.get();
        }

        std::size_t out = 0;
        event::IoEvent wait_event = event::IoEvent::None;
        common::IoErr err = stream_.poll_write(write_buf, group_len, out, wait_event);
        if (err == common::IoErr::None) {
            if (out == 0) {
                co_return total_written;
            }
            // With default SSL_write semantics (no PARTIAL_WRITE) out == group_len;
            // consume_and_compact handles any partial advance correctly regardless.
            buf.consume_and_compact(out);
            total_written += out;
            continue;
        }
        if (err != common::IoErr::WouldBlock) {
            if (total_written != 0) {
                co_return total_written;
            }
            co_return std::unexpected(err);
        }

        auto wait_result = co_await wait_tls_event(stream_, wait_event, deadline);
        if (!wait_result) {
            if (total_written != 0) {
                co_return total_written;
            }
            co_return std::unexpected(wait_result.error());
        }
        // Retry the same group: write_buf (scratch or node pointer) and the chain
        // are unchanged because nothing was consumed while suspended.
    }
}

void TlsTransport::close() { stream_.close(); }

bool TlsTransport::valid() const noexcept { return stream_.valid(); }

int TlsTransport::fd() const noexcept { return stream_.fd(); }

std::string_view TlsTransport::negotiated_alpn() const noexcept { return stream_.selected_alpn(); }

const net::SocketAddress &TlsTransport::remote_addr() const noexcept { return stream_.remote_addr(); }

event::EventLoop &TlsTransport::loop() const noexcept { return stream_.loop(); }

} // namespace fiber::http
