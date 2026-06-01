#include "Http3Connection.h"

#include "../common/Assert.h"

namespace fiber::http {

Http3Connection::Http3Connection(quic::QuicConnection &quic) noexcept : Http3Connection(quic, Options{}) {}

Http3Connection::Http3Connection(quic::QuicConnection &quic, const Options &options) noexcept :
    quic_(&quic), options_(options) {
    FIBER_ASSERT(quic_ != nullptr);
}

common::IoResult<void> Http3Connection::start() noexcept {
    if (state_ != Http3ConnectionState::Init) {
        return std::unexpected(common::IoErr::Already);
    }
    if (quic_->closing()) {
        return std::unexpected(common::IoErr::Canceled);
    }
    state_ = Http3ConnectionState::Running;
    return {};
}

common::IoResult<void> Http3Connection::apply_peer_settings(const Http3Settings &settings) noexcept {
    if (closing()) {
        return std::unexpected(common::IoErr::Canceled);
    }
    if (peer_settings_received_) {
        return std::unexpected(common::IoErr::Already);
    }
    peer_settings_ = settings;
    peer_settings_received_ = true;
    return {};
}

void Http3Connection::graceful_shutdown(Http3ErrorCode error) noexcept {
    if (state_ == Http3ConnectionState::Closed) {
        return;
    }
    close_error_ = error;
    state_ = Http3ConnectionState::Draining;
}

void Http3Connection::close(Http3ErrorCode error) noexcept {
    if (state_ == Http3ConnectionState::Closed) {
        return;
    }
    close_error_ = error;
    state_ = Http3ConnectionState::Closing;
    quic_->close(quic::QuicErrorCode::ApplicationError);
}

void Http3Connection::mark_closed() noexcept { state_ = Http3ConnectionState::Closed; }

} // namespace fiber::http
