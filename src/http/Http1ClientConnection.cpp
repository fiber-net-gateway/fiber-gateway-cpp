#include "Http1ClientConnection.h"

#include <memory>
#include <utility>

#include "../common/Assert.h"
#include "../net/TcpListener.h"
#include "../net/TcpStream.h"
#include "ClientHttp1Exchange.h"
#include "HttpTransport.h"
#include "TlsAlpn.h"

namespace fiber::http {

namespace {

constexpr std::string_view kHttp11Alpn = "http/1.1";

bool supports_http1_alpn(std::string_view alpn) noexcept { return alpn.empty() || alpn == kHttp11Alpn; }

} // namespace

Http1ClientConnectionOptions Http1ClientConnection::normalize_options(Http1ClientConnectionOptions options) noexcept {
    if (options.tls.enabled) {
        normalize_http1_alpn(options.tls);
    }
    return options;
}

Http1ClientConnection::Http1ClientConnection(event::EventLoop &loop, Http1ClientConnectionOptions options) noexcept :
    loop_(&loop), options_(normalize_options(std::move(options))), tls_ctx_(options_.tls, false, false) {}

Http1ClientConnection::~Http1ClientConnection() {
    if (!transport_) {
        return;
    }
    if (loop_ && loop_->in_loop()) {
        close();
        return;
    }
    FIBER_ASSERT(false);
}

fiber::async::Task<common::IoResult<void>> Http1ClientConnection::connect(std::chrono::milliseconds timeout) noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    if (!loop_->in_loop()) {
        co_return std::unexpected(common::IoErr::NotSupported);
    }
    if (state_ != State::Init) {
        co_return std::unexpected(common::IoErr::Busy);
    }

    if (options_.tls.enabled) {
        auto init_result = tls_ctx_.init();
        if (!init_result) {
            co_return std::unexpected(init_result.error());
        }
    }

    auto connect_result = co_await net::TcpStream::connect(*loop_, options_.peer_addr, timeout);
    if (!connect_result) {
        co_return std::unexpected(connect_result.error());
    }

    net::AcceptResult accept(connect_result->release_fd(), connect_result->take_peer());
    std::unique_ptr<HttpTransport> transport;
    if (options_.tls.enabled) {
        auto transport_result = TlsTransport::create(*loop_, std::move(accept), tls_ctx_, options_.tcp);
        if (!transport_result) {
            co_return std::unexpected(transport_result.error());
        }
        transport = std::move(*transport_result);

        auto handshake_result = co_await transport->handshake(tls_ctx_.options().handshake_timeout);
        if (!handshake_result) {
            transport->close();
            co_return std::unexpected(handshake_result.error());
        }

        if (!supports_http1_alpn(transport->negotiated_alpn())) {
            transport->close();
            co_return std::unexpected(common::IoErr::NotSupported);
        }
    } else {
        auto transport_result = TcpTransport::create(*loop_, std::move(accept), options_.tcp);
        if (!transport_result) {
            co_return std::unexpected(transport_result.error());
        }
        transport = std::move(*transport_result);
    }

    if (!transport || !transport->valid()) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    transport_ = std::move(transport);
    state_ = State::ConnectedIdle;
    keepalive_usable_ = true;
    co_return common::IoResult<void>{};
}

void Http1ClientConnection::mark_unusable() noexcept {
    keepalive_usable_ = false;
    active_exchange_ = nullptr;
    state_ = State::Closed;
}

void Http1ClientConnection::close() noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    mark_unusable();
    if (transport_) {
        transport_->close();
        transport_.reset();
    }
}

bool Http1ClientConnection::valid() const noexcept { return transport_ && transport_->valid(); }

bool Http1ClientConnection::idle() const noexcept { return state_ == State::ConnectedIdle && valid(); }

bool Http1ClientConnection::busy() const noexcept { return state_ == State::Busy && valid(); }

bool Http1ClientConnection::connected() const noexcept {
    return (state_ == State::ConnectedIdle || state_ == State::Busy) && valid();
}

bool Http1ClientConnection::reusable() const noexcept { return idle() && keepalive_usable_; }

event::EventLoop &Http1ClientConnection::loop() const noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    return *loop_;
}

bool Http1ClientConnection::acquire_exchange(ClientHttp1Exchange *exchange) noexcept {
    if (!exchange || active_exchange_ || state_ != State::ConnectedIdle || !valid() || !keepalive_usable_) {
        return false;
    }
    active_exchange_ = exchange;
    state_ = State::Busy;
    return true;
}

void Http1ClientConnection::release_exchange(ClientHttp1Exchange *exchange, bool keepalive) noexcept {
    if (active_exchange_ != exchange) {
        return;
    }
    active_exchange_ = nullptr;
    if (!keepalive || !valid()) {
        mark_unusable();
        return;
    }
    keepalive_usable_ = true;
    state_ = State::ConnectedIdle;
}

void Http1ClientConnection::fail_exchange(ClientHttp1Exchange *exchange) noexcept {
    if (active_exchange_ == exchange) {
        active_exchange_ = nullptr;
    }
    mark_unusable();
}

} // namespace fiber::http
