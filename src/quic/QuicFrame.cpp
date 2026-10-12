#include <fiber/quic/QuicFrame.h>

#include <cstring>
#include <expected>
#include <memory>
#include <new>
#include <utility>

namespace fiber::quic {

void QuicOutputFrame::reset() noexcept {
    FIBER_ASSERT(!hook.linked());
    std::destroy_at(this);
    std::construct_at(this);
}

QuicOutputFramePool::~QuicOutputFramePool() { clear(); }

QuicOutputFrame *QuicOutputFramePool::alloc() noexcept {
    if (QuicOutputFrame *frame = free_frames_.pop_front()) {
        --cached_count_;
        return frame;
    }
    return new (std::nothrow) QuicOutputFrame{};
}

void QuicOutputFramePool::release(QuicOutputFrame *frame) noexcept {
    if (frame == nullptr || frame->hook.linked()) {
        return;
    }

    if (cached_count_ < kQuicOutputFramePoolMaxCached) {
        frame->reset();
        free_frames_.push_front(*frame);
        ++cached_count_;
        return;
    }

    delete frame;
}

void QuicOutputFramePool::clear() noexcept {
    while (QuicOutputFrame *frame = free_frames_.pop_front()) {
        delete frame;
    }
    cached_count_ = 0;
}

common::IoResult<void> quic_output_frame_set_owned_data(QuicOutputFrame &frame, const std::uint8_t *data,
                                                        std::size_t len) noexcept {
    if ((data == nullptr && len != 0) || len > UINT32_MAX) {
        return std::unexpected(common::IoErr::Invalid);
    }

    switch (frame.type) {
        case QuicFrameType::Ack:
        case QuicFrameType::AckEcn:
        case QuicFrameType::Crypto:
        case QuicFrameType::NewToken:
        case QuicFrameType::ConnectionClose:
        case QuicFrameType::ConnectionCloseApp:
            break;
        default:
            return std::unexpected(common::IoErr::Invalid);
    }

    if (frame.data) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (len == 0) {
        frame.encoded_len = 0;
        return {};
    }

    mem::IoBuf buf = mem::IoBuf::allocate(len);
    if (!buf) {
        return std::unexpected(common::IoErr::NoMem);
    }
    std::memcpy(buf.writable_data(), data, len);
    buf.commit(len);
    frame.data = std::move(buf);
    frame.encoded_len = 0;
    return {};
}

bool quic_output_frame_ack_eliciting(QuicFrameType type) noexcept {
    return type != QuicFrameType::Padding && type != QuicFrameType::Ack && type != QuicFrameType::AckEcn &&
           type != QuicFrameType::ConnectionClose && type != QuicFrameType::ConnectionCloseApp;
}

bool quic_output_frame_retransmittable_on_loss(QuicFrameType type) noexcept {
    switch (type) {
        case QuicFrameType::Ack:
        case QuicFrameType::AckEcn:
        case QuicFrameType::Padding:
        case QuicFrameType::Ping:
        case QuicFrameType::PathChallenge:
        case QuicFrameType::PathResponse:
        case QuicFrameType::ConnectionClose:
        case QuicFrameType::ConnectionCloseApp:
            return false;

        case QuicFrameType::Crypto:
        case QuicFrameType::ResetStream:
        case QuicFrameType::StopSending:
        case QuicFrameType::NewToken:
        case QuicFrameType::Stream:
        case QuicFrameType::Stream1:
        case QuicFrameType::Stream2:
        case QuicFrameType::Stream3:
        case QuicFrameType::Stream4:
        case QuicFrameType::Stream5:
        case QuicFrameType::Stream6:
        case QuicFrameType::Stream7:
        case QuicFrameType::MaxData:
        case QuicFrameType::MaxStreamData:
        case QuicFrameType::MaxStreamsBidi:
        case QuicFrameType::MaxStreamsUni:
        case QuicFrameType::DataBlocked:
        case QuicFrameType::StreamDataBlocked:
        case QuicFrameType::StreamsBlockedBidi:
        case QuicFrameType::StreamsBlockedUni:
        case QuicFrameType::NewConnectionId:
        case QuicFrameType::RetireConnectionId:
        case QuicFrameType::HandshakeDone:
            return true;
    }
    return false;
}

} // namespace fiber::quic
