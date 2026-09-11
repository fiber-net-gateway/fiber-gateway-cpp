#include <fiber/http/Http3ClientConnection.h>

#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/http/ClientHttp3Exchange.h>
#include "http/Http3ClientConnectionImpl.h"

namespace fiber::http {

Http3ClientConnection::Http3ClientConnection(Http3ClientConnection &&other) noexcept :
    quic_(std::move(other.quic_)), impl_(other.impl_) {
    other.impl_ = nullptr;
}

Http3ClientConnection &Http3ClientConnection::operator=(Http3ClientConnection &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    shutdown();
    quic_ = std::move(other.quic_);
    impl_ = other.impl_;
    other.impl_ = nullptr;
    return *this;
}

Http3ClientConnection::~Http3ClientConnection() { shutdown(); }

ClientHttp3Exchange Http3ClientConnection::open_exchange(mem::BufPool &pool) noexcept {
    return valid() ? ClientHttp3Exchange(*this, pool) : ClientHttp3Exchange{};
}

void Http3ClientConnection::shutdown(Http3ErrorCode error) noexcept {
    if (impl_ != nullptr) {
        impl_->close(error);
    }
}

void Http3ClientConnection::graceful_shutdown(Http3ErrorCode error) noexcept {
    if (impl_ != nullptr) {
        impl_->graceful_shutdown(error);
    }
}

async::Task<void> Http3ClientConnection::wait_closed() noexcept {
    if (impl_ != nullptr) {
        co_await impl_->wait_closed();
    }
}

bool Http3ClientConnection::accepting_requests() const noexcept { return valid() && impl_->accepting_requests(); }
Http3ConnectionState Http3ClientConnection::state() const noexcept {
    FIBER_ASSERT(valid());
    return impl_->state();
}
Http3ErrorCode Http3ClientConnection::close_error() const noexcept {
    FIBER_ASSERT(valid());
    return impl_->close_error();
}
bool Http3ClientConnection::peer_settings_received() const noexcept {
    FIBER_ASSERT(valid());
    return impl_->peer_settings_received();
}
const Http3Settings &Http3ClientConnection::local_settings() const noexcept {
    FIBER_ASSERT(valid());
    return impl_->local_settings();
}
const Http3Settings &Http3ClientConnection::peer_settings() const noexcept {
    FIBER_ASSERT(valid());
    return impl_->peer_settings();
}
bool Http3ClientConnection::peer_goaway_received() const noexcept {
    FIBER_ASSERT(valid());
    return impl_->peer_goaway_received();
}
std::uint64_t Http3ClientConnection::peer_goaway_id() const noexcept {
    FIBER_ASSERT(valid());
    return impl_->peer_goaway_id();
}
quic::QuicConnection &Http3ClientConnection::quic() noexcept {
    FIBER_ASSERT(quic_);
    return *quic_;
}

const quic::QuicConnection &Http3ClientConnection::quic() const noexcept {
    FIBER_ASSERT(quic_);
    return *quic_;
}

} // namespace fiber::http
