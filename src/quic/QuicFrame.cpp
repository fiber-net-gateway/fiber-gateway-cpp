#include <fiber/quic/QuicFrame.h>

#include <cstring>
#include <expected>
#include <new>
#include <utility>

namespace fiber::quic {

void QuicOutputFrameQueue::push_front(QuicOutputFrame &frame) noexcept {
    if (frame.queued) {
        return;
    }
    frame.next = head_;
    frame.prev = nullptr;
    frame.queued = true;
    if (head_ != nullptr) {
        head_->prev = &frame;
    }
    head_ = &frame;
    if (tail_ == nullptr) {
        tail_ = &frame;
    }
}

void QuicOutputFrameQueue::push_back(QuicOutputFrame &frame) noexcept {
    if (frame.queued) {
        return;
    }
    frame.next = nullptr;
    frame.prev = tail_;
    frame.queued = true;
    if (tail_ != nullptr) {
        tail_->next = &frame;
    } else {
        head_ = &frame;
    }
    tail_ = &frame;
}

void QuicOutputFrameQueue::insert_after(QuicOutputFrame &position, QuicOutputFrame &frame) noexcept {
    if (!position.queued || frame.queued) {
        return;
    }
    frame.next = position.next;
    frame.prev = &position;
    frame.queued = true;
    if (frame.next != nullptr) {
        frame.next->prev = &frame;
    }
    position.next = &frame;
    if (tail_ == &position) {
        tail_ = &frame;
    }
}

void QuicOutputFrameQueue::erase(QuicOutputFrame &frame) noexcept {
    QuicOutputFrame *prev = nullptr;
    QuicOutputFrame *current = head_;
    while (current != nullptr) {
        if (current == &frame) {
            erase_after(prev, frame);
            return;
        }
        prev = current;
        current = current->next;
    }
}

void QuicOutputFrameQueue::erase_after(QuicOutputFrame *prev, QuicOutputFrame &frame) noexcept {
    if (!frame.queued || frame.prev != prev) {
        return;
    }
    if (prev != nullptr) {
        if (prev->next != &frame) {
            return;
        }
        prev->next = frame.next;
    } else {
        if (head_ != &frame) {
            return;
        }
        head_ = frame.next;
    }
    if (frame.next != nullptr) {
        frame.next->prev = prev;
    }
    if (tail_ == &frame) {
        tail_ = prev;
    }
    frame.next = nullptr;
    frame.prev = nullptr;
    frame.queued = false;
}

QuicOutputFrame *QuicOutputFrameQueue::pop_front() noexcept {
    QuicOutputFrame *frame = head_;
    if (frame == nullptr) {
        return nullptr;
    }
    erase_after(nullptr, *frame);
    return frame;
}

QuicOutputFrame *QuicOutputFrameQueue::pop_back() noexcept {
    QuicOutputFrame *frame = tail_;
    if (frame == nullptr) {
        return nullptr;
    }
    erase_after(frame->prev, *frame);
    return frame;
}

void QuicOutputFrameQueue::prepend_all(QuicOutputFrameQueue &source) noexcept {
    if (source.head_ == nullptr) {
        return;
    }
    source.tail_->next = head_;
    if (head_ != nullptr) {
        head_->prev = source.tail_;
    }
    head_ = source.head_;
    if (tail_ == nullptr) {
        tail_ = source.tail_;
    }
    source.head_ = nullptr;
    source.tail_ = nullptr;
}

QuicOutputFramePool::~QuicOutputFramePool() { clear(); }

QuicOutputFrame *QuicOutputFramePool::alloc() noexcept {
    if (free_head_ != nullptr) {
        QuicOutputFrame *frame = free_head_;
        free_head_ = frame->next;
        --cached_count_;
        *frame = QuicOutputFrame{};
        return frame;
    }
    return new (std::nothrow) QuicOutputFrame{};
}

void QuicOutputFramePool::release(QuicOutputFrame *frame) noexcept {
    if (frame == nullptr || frame->queued) {
        return;
    }

    *frame = QuicOutputFrame{};
    if (cached_count_ < kQuicOutputFramePoolMaxCached) {
        frame->next = free_head_;
        free_head_ = frame;
        ++cached_count_;
        return;
    }

    delete frame;
}

void QuicOutputFramePool::clear() noexcept {
    QuicOutputFrame *frame = free_head_;
    while (frame != nullptr) {
        QuicOutputFrame *next = frame->next;
        delete frame;
        frame = next;
    }
    free_head_ = nullptr;
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
