#include "Http3ClientConnection.h"

#include <utility>

#include "../common/Assert.h"
#include "ClientHttp3Exchange.h"
#include "Http3Connection.h"

namespace fiber::http {

Http3ClientConnection::Http3ClientConnection(Http3ClientConnection &&other) noexcept :
    quic_(std::move(other.quic_)), h3_(other.h3_) {
    other.h3_ = nullptr;
}

Http3ClientConnection &Http3ClientConnection::operator=(Http3ClientConnection &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    shutdown();
    quic_ = std::move(other.quic_);
    h3_ = other.h3_;
    other.h3_ = nullptr;
    return *this;
}

Http3ClientConnection::~Http3ClientConnection() { shutdown(); }

ClientHttp3Exchange Http3ClientConnection::open_exchange(mem::BufPool &pool) noexcept {
    return valid() ? ClientHttp3Exchange(*this, pool) : ClientHttp3Exchange{};
}

void Http3ClientConnection::shutdown(Http3ErrorCode error) noexcept {
    if (h3_ != nullptr) {
        h3_->close(error);
    }
}

void Http3ClientConnection::graceful_shutdown(Http3ErrorCode error) noexcept {
    if (h3_ != nullptr) {
        h3_->graceful_shutdown(error);
    }
}

async::Task<void> Http3ClientConnection::wait_closed() noexcept {
    if (h3_ != nullptr) {
        co_await h3_->wait_closed();
    }
}

Http3Connection &Http3ClientConnection::http3() noexcept {
    FIBER_ASSERT(h3_ != nullptr);
    return *h3_;
}

const Http3Connection &Http3ClientConnection::http3() const noexcept {
    FIBER_ASSERT(h3_ != nullptr);
    return *h3_;
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
