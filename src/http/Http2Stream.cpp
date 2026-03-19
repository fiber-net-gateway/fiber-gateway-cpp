#include "Http2Stream.h"

#include <array>

#include "../common/Assert.h"
#include "Http2Connection.h"

namespace fiber::http {

namespace {

constexpr std::uint16_t bit(Http2Stream::StreamOp op) noexcept {
    return static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(op));
}

constexpr std::array<std::uint16_t, 7> kValidOpBits = {
    bit(Http2Stream::StreamOp::RecvHeaders) | bit(Http2Stream::StreamOp::RecvHeadersEndStream) |
        bit(Http2Stream::StreamOp::SendHeaders) | bit(Http2Stream::StreamOp::SendHeadersEndStream),
    0,
    0,
    bit(Http2Stream::StreamOp::RecvHeaders) | bit(Http2Stream::StreamOp::RecvHeadersEndStream) |
        bit(Http2Stream::StreamOp::RecvData) | bit(Http2Stream::StreamOp::RecvDataEndStream) |
        bit(Http2Stream::StreamOp::SendHeaders) | bit(Http2Stream::StreamOp::SendHeadersEndStream) |
        bit(Http2Stream::StreamOp::SendData) | bit(Http2Stream::StreamOp::SendDataEndStream),
    bit(Http2Stream::StreamOp::RecvHeaders) | bit(Http2Stream::StreamOp::RecvHeadersEndStream) |
        bit(Http2Stream::StreamOp::RecvData) | bit(Http2Stream::StreamOp::RecvDataEndStream),
    bit(Http2Stream::StreamOp::SendHeaders) | bit(Http2Stream::StreamOp::SendHeadersEndStream) |
        bit(Http2Stream::StreamOp::SendData) | bit(Http2Stream::StreamOp::SendDataEndStream),
    0,
};

} // namespace

common::IoErr Http2Stream::on_header_recv(const mem::IoBuf &payload, std::size_t block_offset, std::size_t length,
                                          bool end_headers, bool end_stream) noexcept {
    common::IoErr err = transition_on_recv_headers(end_stream);
    if (err != common::IoErr::None) {
        return err;
    }

    (void)payload;
    (void)block_offset;
    (void)length;
    (void)end_headers;
    // TODO: decode/process received header block fragments.
    return common::IoErr::None;
}

common::IoErr Http2Stream::on_data_recv(const mem::IoBuf &payload, std::size_t data_offset, std::size_t length,
                                        bool end_stream) noexcept {
    common::IoErr err = transition_on_recv_data(end_stream);
    if (err != common::IoErr::None) {
        return err;
    }

    (void)payload;
    (void)data_offset;
    (void)length;
    // TODO: consume/process received DATA payload.
    return common::IoErr::None;
}

void Http2Stream::on_remote_rst(Http2ErrorCode, common::IoErr result) noexcept { close(result); }

common::IoErr Http2Stream::close_rst(Http2ErrorCode code, common::IoErr result) noexcept {
    if (!conn_) {
        return close_reason_ != common::IoErr::None ? close_reason_ : common::IoErr::Canceled;
    }

    common::IoErr err = conn_->send_rst_stream(stream_id_, code);
    if (err != common::IoErr::None) {
        return err;
    }

    close(result);
    conn_->try_release_stream(*this);
    return common::IoErr::None;
}

void Http2Stream::update_send_window(std::int32_t delta) noexcept { send_window_ += delta; }

bool Http2Stream::is_valid_transition(State state, StreamOp op) noexcept {
    const std::size_t state_index = static_cast<std::size_t>(state);
    FIBER_ASSERT(state_index < kValidOpBits.size());
    return (kValidOpBits[state_index] & bit(op)) != 0;
}

common::IoErr Http2Stream::transition_on_recv_headers(bool end_stream) noexcept {
    const StreamOp op = end_stream ? StreamOp::RecvHeadersEndStream : StreamOp::RecvHeaders;
    if (!is_valid_transition(state_, op)) {
        return common::IoErr::Invalid;
    }

    if (state_ == State::Idle) {
        state_ = end_stream ? State::HalfClosedRemote : State::Open;
    } else if (end_stream) {
        transition_on_remote_end_stream();
    }
    return common::IoErr::None;
}

common::IoErr Http2Stream::transition_on_recv_data(bool end_stream) noexcept {
    const StreamOp op = end_stream ? StreamOp::RecvDataEndStream : StreamOp::RecvData;
    if (!is_valid_transition(state_, op)) {
        return common::IoErr::Invalid;
    }

    if (end_stream) {
        transition_on_remote_end_stream();
    }
    return common::IoErr::None;
}

void Http2Stream::transition_on_remote_end_stream() noexcept {
    switch (state_) {
        case State::Open:
            state_ = State::HalfClosedRemote;
            break;
        case State::HalfClosedLocal:
            state_ = State::Closed;
            break;
        default:
            break;
    }
}

void Http2Stream::transition_on_local_end_stream() noexcept {
    switch (state_) {
        case State::Open:
            state_ = State::HalfClosedLocal;
            break;
        case State::HalfClosedRemote:
            state_ = State::Closed;
            break;
        default:
            break;
    }
}

void Http2Stream::close(common::IoErr result) noexcept {
    if (close_reason_ == common::IoErr::None) {
        close_reason_ = result;
    }
    state_ = State::Closed;
    active_ = false;
}

bool Http2Stream::ready_for_connection_release() const noexcept { return state_ == State::Closed; }

bool Http2Stream::ready_for_destruction() const noexcept {
    return !attached_to_connection_ && ready_for_connection_release() && ref_count_ == 0;
}

void Http2Stream::retain() noexcept { ++ref_count_; }

void Http2Stream::release() noexcept {
    FIBER_ASSERT(ref_count_ != 0);
    --ref_count_;
    if (ready_for_destruction()) {
        delete this;
    }
}

} // namespace fiber::http
