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

} // namespace

QuicStreamSendBuffer::QuicStreamSendBuffer(mem::IoBufNodePool &pool) noexcept : pool_(&pool) {}

QuicStreamSendBuffer::~QuicStreamSendBuffer() { clear_extents(); }

common::IoResult<std::size_t> QuicStreamSendBuffer::append(const mem::IoBuf &buf, bool fin) noexcept {
    if (reset_) {
        return std::unexpected(common::IoErr::BrokenPipe);
    }
    if (fin_acked_) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (fin_appended_ && (buf.readable() > 0 || fin)) {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::size_t bytes = buf.readable();

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

        // Try to merge with tail if same underlying storage and adjacent.
        if (tail_ != nullptr && tail_->buf && slice.same_storage(tail_->buf) &&
            tail_->buf.try_merge_adjacent(std::move(slice))) {
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

common::IoResult<std::size_t> QuicStreamSendBuffer::append_chain(mem::IoBufChain &chain) noexcept {
    if (!chain.bound() || &chain.node_pool() != pool_) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (reset_) {
        return std::unexpected(common::IoErr::BrokenPipe);
    }
    if (fin_acked_) {
        return std::unexpected(common::IoErr::Invalid);
    }

    const bool chain_complete = chain.complete();
    const std::size_t bytes = chain.readable_bytes();
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

        if (tail_ != nullptr && tail_->buf && extent->buf.same_storage(tail_->buf) &&
            tail_->buf.try_merge_adjacent(std::move(extent->buf))) {
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

common::IoResult<QuicStreamSendBuffer::EncodedFrameResult>
QuicStreamSendBuffer::encode_stream_frame(std::uint64_t stream_id, std::uint8_t *dst, std::size_t capacity) noexcept {
    EncodedFrameResult result{};
    if (reset_) {
        return result;
    }

    mem::IoBufNode *cur = ready_head_;

    // No ready data — check if we need to send a fin-only frame.
    if (cur == nullptr) {
        if (!has_pending_fin() || buffered_bytes() > 0) {
            return result;
        }

        const std::uint64_t offset = total_appended_bytes_ - buffered_bytes();
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
        result.fin = true;
        result.encoded = true;
        fin_inflight_ = true;
        return result;
    }

    // Has ready data — encode from cur.
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
            // All data fits with LEN field.
            use_len = true;
            actual_data = data_len;
        } else if (data_len >= remaining) {
            // Data fills remaining exactly — no LEN needed (parser uses leftover bytes).
            use_len = false;
            actual_data = remaining;
        } else if (remaining >= 2) {
            // Need to split — iteratively compute max data with LEN.
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

    // Determine FIN: set if this encodes the last ready byte and fin is pending.
    bool encode_fin = false;
    if (fin_appended_ && !fin_inflight_ && actual_data == data_len && is_last_ready_extent(cur)) {
        encode_fin = true;
    }

    // Split extent if encoding only part of it.
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

    // Encode STREAM frame header + data.
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

    // Update state: ready → inflight.
    cur->state = static_cast<std::uint8_t>(QuicSendExtentState::Inflight);
    ready_bytes_ -= actual_data;
    inflight_bytes_ += actual_data;

    // Advance ready_head_ past inflight extents.
    while (ready_head_ != nullptr && ready_head_->state == static_cast<std::uint8_t>(QuicSendExtentState::Inflight)) {
        ready_head_ = ready_head_->next;
    }

    if (encode_fin) {
        fin_inflight_ = true;
    }

    result.offset = static_cast<std::size_t>(cur->offset);
    result.data_len = actual_data;
    result.encoded_len = wc.offset();
    result.fin = encode_fin;
    result.encoded = true;
    return result;
}

common::IoResult<void> QuicStreamSendBuffer::mark_acked(std::size_t offset, std::size_t length,
                                                        bool encoded_fin) noexcept {
    if (reset_) {
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

            // Unlink from list.
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

    // Recompute ready_head_: scan from head for the first ready extent.
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

common::IoResult<void> QuicStreamSendBuffer::mark_failed(std::size_t offset, std::size_t length,
                                                         bool encoded_fin) noexcept {
    if (reset_) {
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

            // Try to merge with next if same storage (undo a previous split).
            try_merge_with_next(cur);
        }

        cur = next;
    }

    // Recompute ready_head_: find the first ready extent.
    ready_head_ = head_;
    while (ready_head_ != nullptr && ready_head_->state == static_cast<std::uint8_t>(QuicSendExtentState::Inflight)) {
        ready_head_ = ready_head_->next;
    }

    if (encoded_fin) {
        fin_inflight_ = false;
    }

    return {};
}

common::IoResult<std::uint64_t> QuicStreamSendBuffer::mark_reset() noexcept {
    if (reset_) {
        return final_size_;
    }
    if (fin_appended_) {
        return std::unexpected(common::IoErr::Invalid);
    }

    final_size_ = total_appended_bytes_;
    reset_ = true;
    fin_inflight_ = false;
    fin_acked_ = false;
    clear_extents();
    return final_size_;
}

bool QuicStreamSendBuffer::is_last_ready_extent(const mem::IoBufNode *extent) const noexcept {
    if (extent->next != nullptr) {
        return false;
    }
    return ready_head_ == extent || ready_head_ == nullptr;
}

void QuicStreamSendBuffer::clear_extents() noexcept {
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

void QuicStreamSendBuffer::try_merge_with_next(mem::IoBufNode *extent) noexcept {
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

    // Merge succeeded: unlink and release next.
    extent->next = next->next;
    if (next == tail_) {
        tail_ = extent;
    }
    pool_->release(next);
    --active_extent_count_;
}

} // namespace fiber::quic
