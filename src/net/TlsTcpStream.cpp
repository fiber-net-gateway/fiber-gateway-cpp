#include "TlsTcpStream.h"

namespace fiber::net {

TlsTcpStream::TlsTcpStream(fiber::event::EventLoop &loop, int fd, SocketAddress remote_addr) :
    stream_(loop, fd), remote_addr_(std::move(remote_addr)) {}

TlsTcpStream::~TlsTcpStream() {}

common::IoResult<void> TlsTcpStream::init(SSL_CTX *ctx, bool is_server,
                                          detail::TlsStreamFd::ConfigureSslFn configure_ssl, void *configure_ssl_ctx) {
    return stream_.init(ctx, is_server, configure_ssl, configure_ssl_ctx);
}

bool TlsTcpStream::valid() const noexcept { return stream_.valid(); }

int TlsTcpStream::fd() const noexcept { return stream_.fd(); }

fiber::event::EventLoop &TlsTcpStream::loop() const noexcept { return stream_.loop(); }

const SocketAddress &TlsTcpStream::remote_addr() const noexcept { return remote_addr_; }

std::string_view TlsTcpStream::selected_alpn() const noexcept { return stream_.selected_alpn(); }

bool TlsTcpStream::handshake_done() const noexcept { return stream_.handshake_done(); }

bool TlsTcpStream::has_pending_read() const noexcept { return stream_.has_pending_read(); }

fiber::common::IoErr TlsTcpStream::apply_socket_options(const TcpSocketOptions &options) noexcept {
    return detail::apply_tcp_socket_options(fd(), options);
}

void TlsTcpStream::close() { stream_.close(); }

fiber::common::IoErr TlsTcpStream::set_read_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_.set_read_callback(callback, ctx);
}

fiber::common::IoErr TlsTcpStream::set_write_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_.set_write_callback(callback, ctx);
}

fiber::common::IoErr TlsTcpStream::clear_read_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_.clear_read_callback(callback, ctx);
}

fiber::common::IoErr TlsTcpStream::clear_write_callback(ReadyCallback callback, void *ctx) noexcept {
    return stream_.clear_write_callback(callback, ctx);
}

TlsTcpStream::IoTask TlsTcpStream::read(void *buf, size_t len, std::chrono::milliseconds timeout) noexcept {
    return stream_.read(buf, len, timeout);
}

TlsTcpStream::IoTask TlsTcpStream::write(const void *buf, size_t len, std::chrono::milliseconds timeout) noexcept {
    return stream_.write(buf, len, timeout);
}

fiber::common::IoResult<size_t> TlsTcpStream::try_read(void *buf, size_t len) noexcept {
    return stream_.try_read(buf, len);
}

fiber::common::IoResult<size_t> TlsTcpStream::try_write(const void *buf, size_t len) noexcept {
    return stream_.try_write(buf, len);
}

TlsTcpStream::HandshakeTask TlsTcpStream::handshake() { return stream_.handshake(); }

TlsTcpStream::ShutdownTask TlsTcpStream::shutdown() { return stream_.shutdown(); }

detail::StreamFd::WaitReadableAwaiter TlsTcpStream::wait_readable(std::chrono::milliseconds timeout) noexcept {
    return stream_.wait_readable(timeout);
}

detail::StreamFd::WaitWritableAwaiter TlsTcpStream::wait_writable(std::chrono::milliseconds timeout) noexcept {
    return stream_.wait_writable(timeout);
}

fiber::common::IoErr TlsTcpStream::poll_handshake(fiber::event::IoEvent &event) noexcept {
    return stream_.poll_handshake(event);
}

fiber::common::IoErr TlsTcpStream::poll_shutdown(fiber::event::IoEvent &event) noexcept {
    return stream_.poll_shutdown(event);
}

fiber::common::IoErr TlsTcpStream::poll_read(void *buf, size_t len, size_t &out,
                                             fiber::event::IoEvent &event) noexcept {
    return stream_.poll_read(buf, len, out, event);
}

fiber::common::IoErr TlsTcpStream::poll_write(const void *buf, size_t len, size_t &out,
                                              fiber::event::IoEvent &event) noexcept {
    return stream_.poll_write(buf, len, out, event);
}

} // namespace fiber::net
