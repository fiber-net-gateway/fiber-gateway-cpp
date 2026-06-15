#include "QuicStreamSendBuffer.h"

#include <algorithm>
#include <cstring>
#include <expected>
#include <limits>

#include "QuicCursor.h"
#include "QuicTransportCodec.h"

namespace fiber::quic {

namespace {

constexpr std::uint8_t kStreamFrameFin = 0x01;
constexpr std::uint8_t kStreamFrameLen = 0x02;
constexpr std::uint8_t kStreamFrameOff = 0x04;

[[nodiscard]] std::size_t stream_frame_header_len(std::uint64_t stream_id, std::uint64_t offset,
                                                  std::size_t payload_len) noexcept {
    std::size_t len = quic_varint_len(static_cast<std::uint64_t>(QuicFrameType::Stream) | kStreamFrameLen);
    len += quic_varint_len(stream_id);
    if (offset != 0) {
        len += quic_varint_len(offset);
    }
    len += quic_varint_len(payload_len);
    return len;
}

[[nodiscard]] std::size_t max_payload_len(std::uint64_t stream_id, std::uint64_t offset, std::size_t available,
                                          std::size_t capacity) noexcept {
    const std::size_t fixed_len = quic_varint_len(static_cast<std::uint64_t>(QuicFrameType::Stream) | kStreamFrameLen) +
                                  quic_varint_len(stream_id) + (offset != 0 ? quic_varint_len(offset) : 0);
    if (capacity <= fixed_len + quic_varint_len(0)) {
        return 0;
    }

    std::size_t payload_len = std::min(available, capacity - fixed_len - quic_varint_len(0));
    while (payload_len != 0 && fixed_len + quic_varint_len(payload_len) + payload_len > capacity) {
        --payload_len;
    }
    return payload_len;
}

} // namespace

QuicStreamSendBuffer::QuicStreamSendBuffer(QuicStreamDataExtentPool &pool) noexcept : pool_(&pool) {}

QuicStreamSendBuffer::~QuicStreamSendBuffer() { clear(); }

common::IoResult<std::size_t> QuicStreamSendBuffer::append(mem::IoBuf data, bool fin) noexcept {
    const std::size_t len = data.readable();
    if (has_final_size_) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (next_append_offset_ > std::numeric_limits<std::uint64_t>::max() - len) {
        return std::unexpected(common::IoErr::Invalid);
    }

    if (len != 0) {
        auto extent = create_extent(next_append_offset_, next_append_offset_ + len, std::move(data));
        if (!extent) {
            return std::unexpected(extent.error());
        }

        QuicStreamDataExtent *prev_tail = tail_;
        push_back(**extent);
        if (prev_tail != nullptr) {
            (void) try_merge_with_next(prev_tail);
        }
    }

    next_append_offset_ += len;
    if (fin) {
        final_size_ = next_append_offset_;
        has_final_size_ = true;
        fin_state_ = QuicStreamSendFinState::Ready;
    }
    return len;
}

common::IoResult<QuicStreamSendEncodeResult>
QuicStreamSendBuffer::encode_stream_frame(std::uint64_t stream_id, std::uint8_t *dst, std::size_t capacity) noexcept {
    if (dst == nullptr && capacity != 0) {
        return std::unexpected(common::IoErr::Invalid);
    }

    QuicStreamSendEncodeResult result{};
    QuicStreamDataExtent *ready = first_ready();
    if (ready == nullptr) {
        if (fin_state_ != QuicStreamSendFinState::Ready) {
            return result;
        }

        const std::size_t encoded_len = stream_frame_header_len(stream_id, final_size_, 0);
        if (encoded_len > capacity) {
            return result;
        }

        QuicWriteCursor out(dst, capacity);
        std::uint64_t type = static_cast<std::uint64_t>(QuicFrameType::Stream) | kStreamFrameLen | kStreamFrameFin;
        if (final_size_ != 0) {
            type |= kStreamFrameOff;
        }
        auto wrote = quic_write_varint(out, type);
        if (wrote) {
            wrote = quic_write_varint(out, stream_id);
        }
        if (wrote && final_size_ != 0) {
            wrote = quic_write_varint(out, final_size_);
        }
        if (wrote) {
            wrote = quic_write_varint(out, 0);
        }
        if (!wrote) {
            return std::unexpected(wrote.error());
        }

        fin_state_ = QuicStreamSendFinState::Inflight;
        result.offset = final_size_;
        result.encoded_len = out.offset();
        result.fin = true;
        result.encoded = true;
        return result;
    }

    const std::size_t available = static_cast<std::size_t>(ready->end - ready->start);
    const std::size_t payload_len = max_payload_len(stream_id, ready->start, available, capacity);
    if (payload_len == 0) {
        return result;
    }

    const std::uint64_t frame_end = ready->start + payload_len;
    const bool sends_fin = has_final_size_ && fin_state_ == QuicStreamSendFinState::Ready && frame_end == final_size_;
    auto split = split_at(frame_end);
    if (!split) {
        return std::unexpected(split.error());
    }

    QuicWriteCursor out(dst, capacity);
    std::uint64_t type = static_cast<std::uint64_t>(QuicFrameType::Stream) | kStreamFrameLen;
    if (ready->start != 0) {
        type |= kStreamFrameOff;
    }
    if (sends_fin) {
        type |= kStreamFrameFin;
    }

    auto wrote = quic_write_varint(out, type);
    if (wrote) {
        wrote = quic_write_varint(out, stream_id);
    }
    if (wrote && ready->start != 0) {
        wrote = quic_write_varint(out, ready->start);
    }
    if (wrote) {
        wrote = quic_write_varint(out, payload_len);
    }
    if (wrote) {
        wrote = out.write_bytes(ready->view.readable_data(), payload_len);
    }
    if (!wrote) {
        return std::unexpected(wrote.error());
    }

    auto marked = set_range_state(ready->start, payload_len, QuicStreamSendExtentState::Ready,
                                  QuicStreamSendExtentState::Inflight);
    if (!marked) {
        return std::unexpected(marked.error());
    }
    if (sends_fin) {
        fin_state_ = QuicStreamSendFinState::Inflight;
    }

    result.offset = ready->start;
    result.data_len = payload_len;
    result.encoded_len = out.offset();
    result.fin = sends_fin;
    result.encoded = true;
    return result;
}

common::IoResult<void> QuicStreamSendBuffer::mark_acked(std::uint64_t offset, std::size_t len, bool fin) noexcept {
    if (offset > std::numeric_limits<std::uint64_t>::max() - len) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (fin && (fin_state_ != QuicStreamSendFinState::Inflight || offset + len != final_size_)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (len != 0) {
        auto removed = remove_range(offset, len, QuicStreamSendExtentState::Inflight);
        if (!removed) {
            return std::unexpected(removed.error());
        }
    }
    if (fin) {
        fin_state_ = QuicStreamSendFinState::None;
    }
    return {};
}

common::IoResult<void> QuicStreamSendBuffer::mark_failed(std::uint64_t offset, std::size_t len, bool fin) noexcept {
    if (offset > std::numeric_limits<std::uint64_t>::max() - len) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (fin && (fin_state_ != QuicStreamSendFinState::Inflight || offset + len != final_size_)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (len != 0) {
        auto changed =
                set_range_state(offset, len, QuicStreamSendExtentState::Inflight, QuicStreamSendExtentState::Ready);
        if (!changed) {
            return std::unexpected(changed.error());
        }
        merge_around(offset);
    }
    if (fin) {
        fin_state_ = QuicStreamSendFinState::Ready;
    }
    return {};
}

void QuicStreamSendBuffer::clear() noexcept {
    QuicStreamDataExtent *extent = head_;
    while (extent != nullptr) {
        QuicStreamDataExtent *next = extent->next;
        pool_->release(extent);
        extent = next;
    }
    head_ = nullptr;
    tail_ = nullptr;
    next_append_offset_ = 0;
    final_size_ = 0;
    buffered_bytes_ = 0;
    ready_bytes_ = 0;
    inflight_bytes_ = 0;
    active_extent_count_ = 0;
    has_final_size_ = false;
    fin_state_ = QuicStreamSendFinState::None;
}

QuicStreamSendExtentState QuicStreamSendBuffer::extent_state(const QuicStreamDataExtent &extent) noexcept {
    return static_cast<QuicStreamSendExtentState>(extent.state);
}

void QuicStreamSendBuffer::set_extent_state(QuicStreamDataExtent &extent, QuicStreamSendExtentState state) noexcept {
    extent.state = static_cast<std::uint8_t>(state);
}

QuicStreamDataExtent *QuicStreamSendBuffer::find_prev(std::uint64_t offset) noexcept {
    QuicStreamDataExtent *prev = nullptr;
    QuicStreamDataExtent *extent = head_;
    while (extent != nullptr && extent->start <= offset) {
        prev = extent;
        extent = extent->next;
    }
    return prev;
}

QuicStreamDataExtent *QuicStreamSendBuffer::first_ready() noexcept {
    QuicStreamDataExtent *extent = head_;
    while (extent != nullptr) {
        if (extent_state(*extent) == QuicStreamSendExtentState::Ready) {
            return extent;
        }
        extent = extent->next;
    }
    return nullptr;
}

common::IoResult<void> QuicStreamSendBuffer::split_at(std::uint64_t offset) noexcept {
    QuicStreamDataExtent *prev = nullptr;
    QuicStreamDataExtent *extent = head_;
    while (extent != nullptr && extent->end <= offset) {
        prev = extent;
        extent = extent->next;
    }
    if (extent == nullptr || offset <= extent->start || offset >= extent->end) {
        return {};
    }
    if (active_extent_count_ >= kQuicStreamDataMaxActiveExtents) {
        return std::unexpected(common::IoErr::MessageTooLarge);
    }

    QuicStreamDataExtent *right = pool_->alloc();
    if (right == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }

    const std::size_t left_len = static_cast<std::size_t>(offset - extent->start);
    const std::size_t right_len = static_cast<std::size_t>(extent->end - offset);
    mem::IoBuf left_view = extent->view.retain_slice(0, left_len);
    mem::IoBuf right_view = extent->view.retain_slice(left_len, right_len);
    if (!left_view || !right_view) {
        pool_->release(right);
        return std::unexpected(common::IoErr::NoMem);
    }

    right->start = offset;
    right->end = extent->end;
    right->block_index = extent->block_index;
    right->state = extent->state;
    right->view = std::move(right_view);
    extent->end = offset;
    extent->view = std::move(left_view);
    insert_after(extent, *right);
    ++active_extent_count_;
    (void) prev;
    return {};
}

common::IoResult<QuicStreamDataExtent *> QuicStreamSendBuffer::create_extent(std::uint64_t start, std::uint64_t end,
                                                                             mem::IoBuf &&view) noexcept {
    if (start >= end || !view || view.readable() != end - start) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (active_extent_count_ >= kQuicStreamDataMaxActiveExtents) {
        return std::unexpected(common::IoErr::MessageTooLarge);
    }

    QuicStreamDataExtent *extent = pool_->alloc();
    if (extent == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }

    extent->start = start;
    extent->end = end;
    extent->block_index = 0;
    set_extent_state(*extent, QuicStreamSendExtentState::Ready);
    extent->view = std::move(view);
    extent->next = nullptr;
    ++active_extent_count_;

    const std::size_t len = static_cast<std::size_t>(end - start);
    buffered_bytes_ += len;
    ready_bytes_ += len;
    return extent;
}

void QuicStreamSendBuffer::push_back(QuicStreamDataExtent &extent) noexcept {
    extent.next = nullptr;
    if (tail_ != nullptr) {
        tail_->next = &extent;
    } else {
        head_ = &extent;
    }
    tail_ = &extent;
}

void QuicStreamSendBuffer::insert_after(QuicStreamDataExtent *prev, QuicStreamDataExtent &extent) noexcept {
    if (prev == nullptr) {
        extent.next = head_;
        head_ = &extent;
        if (tail_ == nullptr) {
            tail_ = &extent;
        }
        return;
    }

    extent.next = prev->next;
    prev->next = &extent;
    if (tail_ == prev) {
        tail_ = &extent;
    }
}

QuicStreamDataExtent *QuicStreamSendBuffer::try_merge_with_next(QuicStreamDataExtent *extent) noexcept {
    if (extent == nullptr || extent->next == nullptr) {
        return extent;
    }

    QuicStreamDataExtent *right = extent->next;
    if (extent_state(*extent) != QuicStreamSendExtentState::Ready ||
        extent_state(*right) != QuicStreamSendExtentState::Ready || extent->end != right->start ||
        !extent->view.try_merge_adjacent(std::move(right->view))) {
        return extent;
    }

    extent->end = right->end;
    extent->next = right->next;
    if (tail_ == right) {
        tail_ = extent;
    }
    --active_extent_count_;
    pool_->release(right);
    return extent;
}

void QuicStreamSendBuffer::unlink_after(QuicStreamDataExtent *prev, QuicStreamDataExtent &extent) noexcept {
    if (prev == nullptr) {
        head_ = extent.next;
    } else {
        prev->next = extent.next;
    }
    if (tail_ == &extent) {
        tail_ = prev;
    }
    --active_extent_count_;
    pool_->release(&extent);
}

void QuicStreamSendBuffer::merge_around(std::uint64_t offset) noexcept {
    QuicStreamDataExtent *prev = nullptr;
    QuicStreamDataExtent *extent = head_;
    while (extent != nullptr && extent->end <= offset) {
        prev = extent;
        extent = extent->next;
    }
    if (extent != nullptr) {
        (void) try_merge_with_next(extent);
    }
    if (prev != nullptr) {
        (void) try_merge_with_next(prev);
        return;
    }
}

common::IoResult<void> QuicStreamSendBuffer::set_range_state(std::uint64_t offset, std::size_t len,
                                                             QuicStreamSendExtentState from,
                                                             QuicStreamSendExtentState to) noexcept {
    if (len == 0 || offset > std::numeric_limits<std::uint64_t>::max() - len) {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::uint64_t end = offset + len;
    auto split = split_at(offset);
    if (!split) {
        return std::unexpected(split.error());
    }
    split = split_at(end);
    if (!split) {
        return std::unexpected(split.error());
    }

    QuicStreamDataExtent *extent = head_;
    std::uint64_t cursor = offset;
    while (extent != nullptr && extent->end <= offset) {
        extent = extent->next;
    }
    while (extent != nullptr && extent->start < end) {
        if (extent->start != cursor || extent->end > end || extent_state(*extent) != from) {
            return std::unexpected(common::IoErr::Invalid);
        }
        const std::size_t extent_len = static_cast<std::size_t>(extent->end - extent->start);
        set_extent_state(*extent, to);
        if (from == QuicStreamSendExtentState::Ready) {
            ready_bytes_ -= extent_len;
            inflight_bytes_ += extent_len;
        } else {
            inflight_bytes_ -= extent_len;
            ready_bytes_ += extent_len;
        }
        cursor = extent->end;
        extent = extent->next;
    }
    if (cursor != end) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return {};
}

common::IoResult<void> QuicStreamSendBuffer::remove_range(std::uint64_t offset, std::size_t len,
                                                          QuicStreamSendExtentState state) noexcept {
    if (len == 0 || offset > std::numeric_limits<std::uint64_t>::max() - len) {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::uint64_t end = offset + len;
    auto split = split_at(offset);
    if (!split) {
        return std::unexpected(split.error());
    }
    split = split_at(end);
    if (!split) {
        return std::unexpected(split.error());
    }

    QuicStreamDataExtent *prev = nullptr;
    QuicStreamDataExtent *extent = head_;
    while (extent != nullptr && extent->end <= offset) {
        prev = extent;
        extent = extent->next;
    }

    std::uint64_t cursor = offset;
    while (extent != nullptr && extent->start < end) {
        QuicStreamDataExtent *next = extent->next;
        if (extent->start != cursor || extent->end > end || extent_state(*extent) != state) {
            return std::unexpected(common::IoErr::Invalid);
        }

        const std::size_t extent_len = static_cast<std::size_t>(extent->end - extent->start);
        buffered_bytes_ -= extent_len;
        if (state == QuicStreamSendExtentState::Ready) {
            ready_bytes_ -= extent_len;
        } else {
            inflight_bytes_ -= extent_len;
        }
        cursor = extent->end;
        unlink_after(prev, *extent);
        extent = next;
    }
    if (cursor != end) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return {};
}

} // namespace fiber::quic
