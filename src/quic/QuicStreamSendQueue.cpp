#include <fiber/quic/QuicStreamSendQueue.h>

#include <algorithm>
#include <expected>

#include <fiber/common/Assert.h>
#include <fiber/quic/QuicCursor.h>
#include <fiber/quic/QuicTransportCodec.h>

namespace fiber::quic {

namespace {

constexpr std::uint8_t kStreamFrameFin = 0x01;
constexpr std::uint8_t kStreamFrameLen = 0x02;
constexpr std::uint8_t kStreamFrameOff = 0x04;

} // namespace
QuicStreamSendQueue::QuicStreamSendQueue(mem::IoBufNodePool &pool) noexcept : QuicStreamSendQueue(pool, Options{}) {}

QuicStreamSendQueue::QuicStreamSendQueue(mem::IoBufNodePool &pool, Options options) noexcept :
    pool_(&pool), buffer_limit_(options.buffer_limit) {}

void QuicStreamSendQueue::init(mem::IoBufNodePool &pool) noexcept { init(pool, Options{}); }

void QuicStreamSendQueue::init(mem::IoBufNodePool &pool, Options options) noexcept {
    FIBER_ASSERT(pool_ == nullptr);
    FIBER_ASSERT(head_ == nullptr);
    FIBER_ASSERT(tail_ == nullptr);
    FIBER_ASSERT(ready_head_ == nullptr);
    FIBER_ASSERT(ready_bytes_ == 0);
    FIBER_ASSERT(inflight_bytes_ == 0);
    FIBER_ASSERT(active_extent_count_ == 0);
    pool_ = &pool;
    buffer_limit_ = options.buffer_limit;
}

QuicStreamSendQueue::~QuicStreamSendQueue() { clear_extents(); }

common::IoResult<std::size_t> QuicStreamSendQueue::try_append(const mem::IoBuf &buf, bool fin) noexcept {
    const std::size_t bytes = buf.readable();
    auto checked = check_append_preconditions(bytes);
    if (!checked) {
        return std::unexpected(checked.error());
    }

    if (fin_acked_) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (fin_appended_ && (bytes > 0 || fin)) {
        return std::unexpected(common::IoErr::Invalid);
    }

    if (bytes > 0) {
        mem::IoBuf slice = buf.retain_slice(0, bytes);
        if (!slice) {
            return std::unexpected(common::IoErr::NoMem);
        }

        mem::IoBufNode *extent = pool_->alloc();
        if (extent == nullptr) {
            return std::unexpected(common::IoErr::NoMem);
        }

        extent->offset = total_appended_bytes_;
        extent->state = static_cast<std::uint8_t>(QuicSendExtentState::Ready);

        if (tail_ != nullptr && tail_->state == static_cast<std::uint8_t>(QuicSendExtentState::Ready) && tail_->buf &&
            slice.same_storage(tail_->buf) && tail_->buf.try_merge_adjacent(std::move(slice))) {
            pool_->release(extent);
        } else {
            extent->buf = std::move(slice);
            extent->next = nullptr;
            if (tail_ != nullptr) {
                tail_->next = extent;
            } else {
                head_ = extent;
            }
            tail_ = extent;
            if (ready_head_ == nullptr) {
                ready_head_ = extent;
            }
            ++active_extent_count_;
        }

        ready_bytes_ += bytes;
        total_appended_bytes_ += bytes;
    }

    if (fin) {
        fin_appended_ = true;
        final_size_ = total_appended_bytes_;
    }

    return bytes;
}

common::IoResult<std::size_t> QuicStreamSendQueue::try_append_chain(mem::IoBufChain &chain) noexcept {
    if (!chain.bound() || &chain.node_pool() != pool_) {
        return std::unexpected(common::IoErr::Invalid);
    }

    const bool chain_complete = chain.complete();
    const std::size_t bytes = chain.readable_bytes();
    auto checked = check_append_preconditions(bytes);
    if (!checked) {
        return std::unexpected(checked.error());
    }

    if (fin_acked_) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (fin_appended_ && (bytes > 0 || chain_complete)) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::size_t appended = 0;
    for (;;) {
        mem::IoBufNode *extent = chain.pop_front_node();
        if (extent == nullptr) {
            break;
        }

        const std::size_t readable = extent->buf.readable();
        if (readable == 0) {
            pool_->release(extent);
            continue;
        }

        extent->offset = total_appended_bytes_;
        extent->state = static_cast<std::uint8_t>(QuicSendExtentState::Ready);
        extent->next = nullptr;

        if (tail_ != nullptr && tail_->state == static_cast<std::uint8_t>(QuicSendExtentState::Ready) && tail_->buf &&
            extent->buf.same_storage(tail_->buf) && tail_->buf.try_merge_adjacent(std::move(extent->buf))) {
            pool_->release(extent);
        } else {
            if (tail_ != nullptr) {
                tail_->next = extent;
            } else {
                head_ = extent;
            }
            tail_ = extent;
            if (ready_head_ == nullptr) {
                ready_head_ = extent;
            }
            ++active_extent_count_;
        }

        ready_bytes_ += readable;
        total_appended_bytes_ += readable;
        appended += readable;
    }

    if (chain_complete) {
        fin_appended_ = true;
        final_size_ = total_appended_bytes_;
        chain.clear_complete();
    }

    return appended;
}

common::IoResult<QuicStreamSendQueue::EncodedFrameResult>
QuicStreamSendQueue::encode_stream_frame(std::uint64_t stream_id, std::uint8_t *dst, std::size_t capacity) noexcept {
    EncodedFrameResult result{};
    if (reset_sent_) {
        return result;
    }

    mem::IoBufNode *cur = ready_head_;

    if (cur == nullptr) {
        if (!has_pending_fin() || buffered_bytes() > 0) {
            return result;
        }

        const std::uint64_t offset = final_size_;
        std::size_t base_hdr = 1 + quic_varint_len(stream_id);
        if (offset > 0) {
            base_hdr += quic_varint_len(offset);
        }
        if (capacity < base_hdr) {
            return result;
        }

        QuicWriteCursor wc(dst, capacity);
        std::uint8_t type_byte = 0x08;
        if (offset > 0) {
            type_byte |= kStreamFrameOff;
        }
        type_byte |= kStreamFrameFin;
        auto r = wc.write_u8(type_byte);
        if (!r) {
            return std::unexpected(r.error());
        }
        r = quic_write_varint(wc, stream_id);
        if (!r) {
            return std::unexpected(r.error());
        }
        if (offset > 0) {
            r = quic_write_varint(wc, offset);
            if (!r) {
                return std::unexpected(r.error());
            }
        }

        result.offset = static_cast<std::size_t>(offset);
        result.data_len = 0;
        result.encoded_len = wc.offset();
        result.has_length = false;
        result.fin = true;
        result.encoded = true;
        fin_inflight_ = true;
        return result;
    }

    const std::size_t data_len = cur->buf.readable();
    std::size_t base_hdr = 1 + quic_varint_len(stream_id);
    if (cur->offset > 0) {
        base_hdr += quic_varint_len(cur->offset);
    }

    bool use_len = false;
    std::size_t actual_data = data_len;

    if (base_hdr < capacity) {
        const std::size_t remaining = capacity - base_hdr;
        const std::size_t len_bytes = quic_varint_len(data_len);

        if (len_bytes + data_len <= remaining) {
            use_len = true;
            actual_data = data_len;
        } else if (data_len >= remaining) {
            use_len = false;
            actual_data = remaining;
        } else if (remaining >= 2) {
            use_len = true;
            std::size_t max_data = remaining - 1;
            for (int i = 0; i < 2; ++i) {
                std::size_t revised = remaining - quic_varint_len(max_data);
                if (revised == max_data) {
                    break;
                }
                max_data = revised;
            }
            if (max_data == 0) {
                return result;
            }
            actual_data = std::min(max_data, data_len);
        } else {
            return result;
        }
    } else {
        return result;
    }

    const bool encode_fin =
            fin_appended_ && !fin_inflight_ && cur->offset <= final_size_ && actual_data == final_size_ - cur->offset;

    if (actual_data < data_len) {
        mem::IoBufNode *remainder = pool_->alloc();
        if (remainder == nullptr) {
            return std::unexpected(common::IoErr::NoMem);
        }
        remainder->offset = cur->offset + actual_data;
        remainder->state = static_cast<std::uint8_t>(QuicSendExtentState::Ready);
        remainder->buf = cur->buf.retain_slice(actual_data, data_len - actual_data);
        remainder->next = cur->next;
        cur->buf = cur->buf.retain_slice(0, actual_data);
        cur->next = remainder;
        if (cur == tail_) {
            tail_ = remainder;
        }
        ++active_extent_count_;
    }

    QuicWriteCursor wc(dst, capacity);
    std::uint8_t type_byte = 0x08;
    if (cur->offset > 0) {
        type_byte |= kStreamFrameOff;
    }
    if (use_len) {
        type_byte |= kStreamFrameLen;
    }
    if (encode_fin) {
        type_byte |= kStreamFrameFin;
    }

    auto r = wc.write_u8(type_byte);
    if (!r) {
        return std::unexpected(r.error());
    }
    r = quic_write_varint(wc, stream_id);
    if (!r) {
        return std::unexpected(r.error());
    }
    if (cur->offset > 0) {
        r = quic_write_varint(wc, cur->offset);
        if (!r) {
            return std::unexpected(r.error());
        }
    }
    if (use_len) {
        r = quic_write_varint(wc, actual_data);
        if (!r) {
            return std::unexpected(r.error());
        }
    }
    r = wc.write_bytes(cur->buf.readable_data(), actual_data);
    if (!r) {
        return std::unexpected(r.error());
    }

    cur->state = static_cast<std::uint8_t>(QuicSendExtentState::Inflight);
    ready_bytes_ -= actual_data;
    inflight_bytes_ += actual_data;

    while (ready_head_ != nullptr && ready_head_->state == static_cast<std::uint8_t>(QuicSendExtentState::Inflight)) {
        ready_head_ = ready_head_->next;
    }

    if (encode_fin) {
        fin_inflight_ = true;
    }

    result.offset = static_cast<std::size_t>(cur->offset);
    result.data_len = actual_data;
    result.encoded_len = wc.offset();
    result.has_length = use_len;
    result.fin = encode_fin;
    result.encoded = true;
    return result;
}

common::IoResult<void> QuicStreamSendQueue::mark_acked(std::size_t offset, std::size_t length,
                                                       bool encoded_fin) noexcept {
    if (reset_sent_) {
        return {};
    }

    const std::size_t end = offset + length;
    mem::IoBufNode *prev = nullptr;
    mem::IoBufNode *cur = head_;

    while (cur != nullptr) {
        mem::IoBufNode *next = cur->next;
        const std::size_t extent_end = static_cast<std::size_t>(cur->offset) + cur->buf.readable();

        if (cur->state == static_cast<std::uint8_t>(QuicSendExtentState::Inflight) &&
            static_cast<std::size_t>(cur->offset) >= offset && extent_end <= end) {

            if (prev != nullptr) {
                prev->next = next;
            } else {
                head_ = next;
            }
            if (cur == tail_) {
                tail_ = prev;
            }

            inflight_bytes_ -= cur->buf.readable();
            pool_->release(cur);
            --active_extent_count_;
        } else {
            prev = cur;
        }

        cur = next;
    }

    ready_head_ = head_;
    while (ready_head_ != nullptr && ready_head_->state == static_cast<std::uint8_t>(QuicSendExtentState::Inflight)) {
        ready_head_ = ready_head_->next;
    }

    if (encoded_fin) {
        fin_acked_ = true;
        fin_inflight_ = false;
    }

    return {};
}

common::IoResult<void> QuicStreamSendQueue::mark_failed(std::size_t offset, std::size_t length,
                                                        bool encoded_fin) noexcept {
    if (reset_sent_) {
        return {};
    }

    const std::size_t end = offset + length;
    mem::IoBufNode *cur = head_;

    while (cur != nullptr) {
        mem::IoBufNode *next = cur->next;
        const std::size_t extent_end = static_cast<std::size_t>(cur->offset) + cur->buf.readable();

        if (cur->state == static_cast<std::uint8_t>(QuicSendExtentState::Inflight) &&
            static_cast<std::size_t>(cur->offset) >= offset && extent_end <= end) {

            cur->state = static_cast<std::uint8_t>(QuicSendExtentState::Ready);
            const std::size_t bytes = cur->buf.readable();
            inflight_bytes_ -= bytes;
            ready_bytes_ += bytes;

            try_merge_with_next(cur);
        }

        cur = next;
    }

    ready_head_ = head_;
    while (ready_head_ != nullptr && ready_head_->state == static_cast<std::uint8_t>(QuicSendExtentState::Inflight)) {
        ready_head_ = ready_head_->next;
    }

    if (encoded_fin) {
        fin_inflight_ = false;
    }

    return {};
}

common::IoResult<std::uint64_t> QuicStreamSendQueue::reset(std::uint64_t error_code) noexcept {
    if (reset_sent_) {
        return final_size_;
    }

    final_size_ = total_appended_bytes_;
    reset_error_code_ = error_code;
    reset_sent_ = true;
    fin_inflight_ = false;
    fin_acked_ = false;
    clear_extents();
    return final_size_;
}

std::size_t QuicStreamSendQueue::buffer_available() const noexcept {
    if (buffered_bytes() >= buffer_limit_) {
        return 0;
    }
    return buffer_limit_ - buffered_bytes();
}

common::IoErr QuicStreamSendQueue::terminal_append_error() const noexcept {
    if (reset_sent_) {
        return common::IoErr::BrokenPipe;
    }
    return common::IoErr::None;
}

common::IoResult<void> QuicStreamSendQueue::check_append_preconditions(std::size_t bytes) const noexcept {
    const common::IoErr terminal = terminal_append_error();
    if (terminal != common::IoErr::None) {
        return std::unexpected(terminal);
    }
    if (bytes > buffer_limit_) {
        return std::unexpected(common::IoErr::MessageTooLarge);
    }
    if (buffer_available() < bytes) {
        return std::unexpected(common::IoErr::WouldBlock);
    }
    return {};
}

void QuicStreamSendQueue::clear_extents() noexcept {
    if (pool_ == nullptr) {
        return;
    }
    mem::IoBufNode *cur = head_;
    while (cur != nullptr) {
        mem::IoBufNode *next = cur->next;
        pool_->release(cur);
        cur = next;
    }
    head_ = nullptr;
    tail_ = nullptr;
    ready_head_ = nullptr;
    ready_bytes_ = 0;
    inflight_bytes_ = 0;
    active_extent_count_ = 0;
}

void QuicStreamSendQueue::try_merge_with_next(mem::IoBufNode *extent) noexcept {
    mem::IoBufNode *next = extent->next;
    if (next == nullptr) {
        return;
    }
    if (extent->state != static_cast<std::uint8_t>(QuicSendExtentState::Ready) ||
        next->state != static_cast<std::uint8_t>(QuicSendExtentState::Ready)) {
        return;
    }
    if (static_cast<std::size_t>(extent->offset) + extent->buf.readable() != static_cast<std::size_t>(next->offset)) {
        return;
    }
    if (!extent->buf.same_storage(next->buf)) {
        return;
    }
    if (!extent->buf.try_merge_adjacent(std::move(next->buf))) {
        return;
    }

    extent->next = next->next;
    if (next == tail_) {
        tail_ = extent;
    }
    pool_->release(next);
    --active_extent_count_;
}

} // namespace fiber::quic
