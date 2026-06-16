#include "QuicStreamReassembler.h"

#include <algorithm>
#include <cstring>
#include <expected>
#include <limits>

#include "../common/Assert.h"

namespace fiber::quic {

// Block size is 64 KiB = 2^16, so block index = offset >> 16.
static constexpr std::size_t kQuicStreamRecvBlockSize = 64 * 1024;
static constexpr std::size_t kQuicStreamRecvMaxActiveExtents = 4096;
static constexpr std::size_t kQuicStreamRecvMaxActiveBlocks = 1024;
static constexpr unsigned kBlockSizeShift = 16;
static constexpr std::uint64_t kBlockOffsetMask = kQuicStreamRecvBlockSize - 1;

QuicStreamReassembler::QuicStreamReassembler(mem::IoBufNodePool &pool) noexcept : pool_(&pool) {}

QuicStreamReassembler::~QuicStreamReassembler() { clear(); }

common::IoResult<std::size_t> QuicStreamReassembler::insert(std::uint64_t offset, QuicSlice data, bool fin) noexcept {
    if ((data.data == nullptr && data.len != 0) || offset > std::numeric_limits<std::uint64_t>::max() - data.len)
            [[unlikely]] {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::uint64_t data_end = offset + data.len;
    if (has_final_size_ && data_end > final_size_) [[unlikely]] {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (fin) {
        if (has_final_size_ && final_size_ != data_end) [[unlikely]] {
            return std::unexpected(common::IoErr::Invalid);
        }
        final_size_ = data_end;
        has_final_size_ = true;
    }
    received_end_offset_ = std::max(received_end_offset_, data_end);

    if (data_end <= next_read_offset_ || data.len == 0) {
        return 0;
    }

    // Trim already-delivered prefix.
    if (offset < next_read_offset_) {
        const auto skip = static_cast<std::size_t>(next_read_offset_ - offset);
        data.data += skip;
        data.len -= skip;
        offset = next_read_offset_;
    }

    const std::uint64_t base_offset = offset;
    std::uint64_t cursor = offset;
    std::size_t copied = 0;

    // --- Single forward walk ---
    //
    // Maintain (prev, cur) where prev->offset <= cursor and cur is the next
    // extent to examine.  Because cursor is monotonically non-decreasing and
    // the list is sorted by offset, we never need to scan backwards.

    mem::IoBufNode *prev = nullptr;
    mem::IoBufNode *cur = head_;

    // Use last_insert_ as a search hint: only valid when the hint starts
    // at or before cursor (so we don't skip extents that precede the hint).
    if (last_insert_ != nullptr && last_insert_->offset <= cursor) {
        prev = last_insert_;
        cur = last_insert_->next;
    }

    while (cursor < data_end) {
        // Advance past extents that already cover the cursor position.
        while (cur != nullptr && cur->offset <= cursor) {
            const std::uint64_t cur_end = cur->offset + cur->buf.readable();
            if (cur_end > cursor) {
                cursor = std::min(cur_end, data_end);
                if (cursor >= data_end) {
                    return copied;
                }
            }
            prev = cur;
            cur = cur->next;
        }

        // After a forward merge in the previous iteration, prev may have been
        // extended past cursor.  Skip forward if so.
        if (prev != nullptr) {
            const std::uint64_t prev_end = prev->offset + prev->buf.readable();
            if (prev_end > cursor) {
                cursor = std::min(prev_end, data_end);
                if (cursor >= data_end) {
                    return copied;
                }
                continue;
            }
        }

        // cur is either null or starts after cursor — there is a gap to fill.
        const std::uint64_t next_start = cur != nullptr ? cur->offset : data_end;
        const std::uint64_t hole_end = std::min({next_start, data_end, block_end(cursor)});
        if (hole_end <= cursor) [[unlikely]] {
            return std::unexpected(common::IoErr::Invalid);
        }

        const auto *src = data.data + static_cast<std::size_t>(cursor - base_offset);
        const std::size_t hole_len = static_cast<std::size_t>(hole_end - cursor);
        const std::uint64_t block = block_of(cursor);

        auto result = create_extent(cursor, hole_len, prev, cur, src, block);
        if (!result) [[unlikely]] {
            return std::unexpected(result.error());
        }

        mem::IoBufNode *new_ext = *result;
        insert_after(prev, *new_ext);

        // Backward merge: try merging prev → new_ext.
        if (prev != nullptr && block_of(prev->offset) == block) {
            (void) try_merge_with_next(prev);
        }

        // Forward merge: if prev absorbed new_ext, try extending further into cur;
        // otherwise try merging new_ext → cur.
        if (prev != nullptr && prev->next != new_ext) {
            // Backward merge succeeded — prev now points to cur (skipping released new_ext).
            (void) try_merge_with_next(prev);
        } else {
            (void) try_merge_with_next(new_ext);
            prev = new_ext;
        }

        // Advance cursor past the filled hole.
        cursor = hole_end;
        copied += hole_len;

        // prev is now the last extent in the chain; cur follows it.
        cur = prev->next;
        last_insert_ = prev;
    }

    return copied;
}

common::IoResult<std::size_t> QuicStreamReassembler::take(std::size_t max_bytes, mem::IoBufChain &out) noexcept {
    FIBER_ASSERT(&out.node_pool() == pool_);
    std::size_t taken = 0;
    while (max_bytes != 0 && head_ != nullptr && head_->offset == next_read_offset_) {
        mem::IoBufNode *extent = head_;
        const std::size_t readable = extent->buf.readable();
        const std::size_t take_bytes = std::min(readable, max_bytes);

        if (take_bytes == readable) [[likely]] {
            const std::uint64_t next_read = extent->offset + readable;
            buffered_bytes_ -= readable;
            max_bytes -= readable;
            taken += readable;
            if (last_insert_ == extent) {
                last_insert_ = nullptr;
            }
            unlink_after(nullptr, *extent);
            next_read_offset_ = next_read;
            if (!out.append_node(extent)) {
                return std::unexpected(common::IoErr::NoMem);
            }
            continue;
        }

        mem::IoBuf piece = extent->buf.retain_slice(0, take_bytes);
        if (!piece || !out.append(std::move(piece))) {
            return std::unexpected(common::IoErr::NoMem);
        }
        extent->buf.consume(take_bytes);
        extent->offset += take_bytes;
        next_read_offset_ += take_bytes;
        buffered_bytes_ -= take_bytes;
        taken += take_bytes;
        break;
    }

    if (finished()) {
        out.mark_complete();
    }
    return taken;
}

void QuicStreamReassembler::clear() noexcept {
    mem::IoBufNode *extent = head_;
    while (extent != nullptr) {
        mem::IoBufNode *next = extent->next;
        pool_->release(extent);
        extent = next;
    }
    head_ = nullptr;
    tail_ = nullptr;
    last_insert_ = nullptr;
    buffered_bytes_ = 0;
    active_extent_count_ = 0;
    active_block_count_ = 0;
    next_read_offset_ = 0;
    received_end_offset_ = 0;
    final_size_ = 0;
    has_final_size_ = false;
}

// --- Block helpers ---

std::uint64_t QuicStreamReassembler::block_of(std::uint64_t offset) noexcept { return offset >> kBlockSizeShift; }

std::size_t QuicStreamReassembler::block_offset(std::uint64_t offset) noexcept {
    return static_cast<std::size_t>(offset & kBlockOffsetMask);
}

std::uint64_t QuicStreamReassembler::block_end(std::uint64_t offset) noexcept {
    return (offset & ~kBlockOffsetMask) + kQuicStreamRecvBlockSize;
}

// --- Extent lifecycle ---

common::IoResult<mem::IoBufNode *> QuicStreamReassembler::create_extent(std::uint64_t offset, std::size_t len,
                                                                        mem::IoBufNode *prev, mem::IoBufNode *next,
                                                                        const std::uint8_t *src,
                                                                        std::uint64_t block) noexcept {
    if (len == 0 || src == nullptr) [[unlikely]] {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (active_extent_count_ >= kQuicStreamRecvMaxActiveExtents) [[unlikely]] {
        return std::unexpected(common::IoErr::MessageTooLarge);
    }

    const bool reuse_prev = prev != nullptr && block_of(prev->offset) == block;
    const bool reuse_next = next != nullptr && block_of(next->offset) == block;
    const bool new_block = !reuse_prev && !reuse_next;
    if (new_block && active_block_count_ >= kQuicStreamRecvMaxActiveBlocks) [[unlikely]] {
        return std::unexpected(common::IoErr::MessageTooLarge);
    }

    mem::IoBufNode *extent = pool_->alloc();
    if (extent == nullptr) [[unlikely]] {
        return std::unexpected(common::IoErr::NoMem);
    }

    const std::size_t local = block_offset(offset);
    mem::IoBuf view{};
    if (reuse_prev || reuse_next) {
        mem::IoBuf &owner = reuse_prev ? prev->buf : next->buf;
        std::memcpy(owner.data() + local, src, len);
        view = owner.retain_storage_slice(local, len);
    } else {
        mem::IoBuf storage = mem::IoBuf::allocate(kQuicStreamRecvBlockSize);
        if (!storage) [[unlikely]] {
            pool_->release(extent);
            return std::unexpected(common::IoErr::NoMem);
        }
        std::memcpy(storage.data() + local, src, len);
        view = storage.retain_storage_slice(local, len);
        ++active_block_count_;
    }

    extent->offset = offset;
    extent->buf = std::move(view);
    extent->next = nullptr;
    ++active_extent_count_;
    buffered_bytes_ += len;
    return extent;
}

void QuicStreamReassembler::insert_after(mem::IoBufNode *prev, mem::IoBufNode &extent) noexcept {
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

mem::IoBufNode *QuicStreamReassembler::try_merge_with_next(mem::IoBufNode *extent) noexcept {
    if (extent == nullptr || extent->next == nullptr) {
        return extent;
    }

    mem::IoBufNode *right = extent->next;
    if (block_of(extent->offset) != block_of(right->offset) || !extent->buf.try_merge_adjacent(std::move(right->buf))) {
        return extent;
    }

    // Merge succeeded: extent absorbs right.
    extent->next = right->next;
    if (tail_ == right) {
        tail_ = extent;
    }
    if (last_insert_ == right) {
        last_insert_ = extent;
    }
    --active_extent_count_;
    pool_->release(right);
    return extent;
}

void QuicStreamReassembler::unlink_after(mem::IoBufNode *prev, mem::IoBufNode &extent) noexcept {
    const std::uint64_t block = block_of(extent.offset);
    const bool has_same_block = has_same_block_neighbor(prev, extent.next, block);
    if (prev == nullptr) {
        head_ = extent.next;
    } else {
        prev->next = extent.next;
    }
    if (tail_ == &extent) {
        tail_ = prev;
    }
    if (last_insert_ == &extent) {
        last_insert_ = prev;
    }
    if (!has_same_block) {
        --active_block_count_;
    }
    --active_extent_count_;
}

bool QuicStreamReassembler::has_same_block_neighbor(const mem::IoBufNode *prev, const mem::IoBufNode *next,
                                                    std::uint64_t block) noexcept {
    return (prev != nullptr && block_of(prev->offset) == block) || (next != nullptr && block_of(next->offset) == block);
}

} // namespace fiber::quic
