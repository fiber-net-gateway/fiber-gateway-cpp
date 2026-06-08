#include "QuicStreamReassembler.h"

#include <algorithm>
#include <cstring>
#include <expected>
#include <limits>
#include <new>

namespace fiber::quic {

QuicRecvExtentPool::~QuicRecvExtentPool() { clear(); }

QuicRecvExtent *QuicRecvExtentPool::alloc() noexcept {
    if (free_head_ != nullptr) {
        QuicRecvExtent *extent = free_head_;
        free_head_ = extent->next;
        --cached_count_;
        extent->start = 0;
        extent->end = 0;
        extent->block_index = 0;
        extent->view = {};
        extent->next = nullptr;
        return extent;
    }
    return new (std::nothrow) QuicRecvExtent{};
}

void QuicRecvExtentPool::release(QuicRecvExtent *extent) noexcept {
    if (extent == nullptr) {
        return;
    }

    extent->start = 0;
    extent->end = 0;
    extent->block_index = 0;
    extent->view = {};
    if (cached_count_ < kQuicRecvExtentPoolMaxCached) {
        extent->next = free_head_;
        free_head_ = extent;
        ++cached_count_;
        return;
    }

    delete extent;
}

void QuicRecvExtentPool::clear() noexcept {
    QuicRecvExtent *extent = free_head_;
    while (extent != nullptr) {
        QuicRecvExtent *next = extent->next;
        delete extent;
        extent = next;
    }
    free_head_ = nullptr;
    cached_count_ = 0;
}

QuicStreamReassembler::QuicStreamReassembler(QuicRecvExtentPool &pool) noexcept : pool_(&pool) {}

QuicStreamReassembler::~QuicStreamReassembler() { clear(); }

common::IoResult<std::size_t> QuicStreamReassembler::insert(std::uint64_t offset, QuicSlice data, bool fin) noexcept {
    if ((data.data == nullptr && data.len != 0) || offset > std::numeric_limits<std::uint64_t>::max() - data.len) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::uint64_t end = offset + data.len;
    if (has_final_size_ && end > final_size_) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (fin) {
        if (has_final_size_ && final_size_ != end) {
            return std::unexpected(common::IoErr::Invalid);
        }
        final_size_ = end;
        has_final_size_ = true;
    }

    if (end <= next_read_offset_ || data.len == 0) {
        return 0;
    }
    if (offset < next_read_offset_) {
        const std::uint64_t skip64 = next_read_offset_ - offset;
        const std::size_t skip = static_cast<std::size_t>(skip64);
        data.data += skip;
        data.len -= skip;
        offset = next_read_offset_;
    }

    const std::uint64_t base_offset = offset;
    std::uint64_t cursor = offset;
    std::size_t copied = 0;

    while (cursor < end) {
        QuicRecvExtent *prev = find_prev(cursor);
        QuicRecvExtent *next = prev != nullptr ? prev->next : head_;

        if (prev != nullptr && prev->end > cursor) {
            cursor = std::min(prev->end, end);
            continue;
        }
        if (next != nullptr && next->start == cursor) {
            cursor = std::min(next->end, end);
            continue;
        }

        std::uint64_t hole_end = end;
        if (next != nullptr && next->start < hole_end) {
            hole_end = next->start;
        }
        hole_end = std::min(hole_end, block_end(cursor));
        if (hole_end <= cursor) {
            return std::unexpected(common::IoErr::Invalid);
        }

        const auto *src = data.data + static_cast<std::size_t>(cursor - base_offset);
        auto extent = create_extent(cursor, hole_end, prev, next, src);
        if (!extent) {
            return std::unexpected(extent.error());
        }

        insert_after(prev, **extent);
        QuicRecvExtent *left = *extent;
        if (prev != nullptr && prev->end == left->start) {
            left = try_merge_with_next(prev);
        }
        left = try_merge_with_next(left);
        last_insert_ = left;

        copied += static_cast<std::size_t>(hole_end - cursor);
        cursor = hole_end;
    }

    return copied;
}

common::IoResult<std::size_t> QuicStreamReassembler::take(std::size_t max_bytes, mem::IoBufChain &out) noexcept {
    std::size_t taken = 0;
    while (max_bytes != 0 && head_ != nullptr && head_->start == next_read_offset_) {
        QuicRecvExtent *extent = head_;
        const std::size_t readable = static_cast<std::size_t>(extent->end - extent->start);
        const std::size_t take_bytes = std::min(readable, max_bytes);

        if (take_bytes == readable) {
            if (!out.append(std::move(extent->view))) {
                return std::unexpected(common::IoErr::NoMem);
            }
            next_read_offset_ = extent->end;
            buffered_bytes_ -= readable;
            max_bytes -= readable;
            taken += readable;
            unlink_after(nullptr, *extent);
            continue;
        }

        mem::IoBuf piece = extent->view.retain_slice(0, take_bytes);
        if (!piece || !out.append(std::move(piece))) {
            return std::unexpected(common::IoErr::NoMem);
        }
        extent->view.consume(take_bytes);
        extent->start += take_bytes;
        next_read_offset_ += take_bytes;
        buffered_bytes_ -= take_bytes;
        taken += take_bytes;
        break;
    }

    return taken;
}

void QuicStreamReassembler::clear() noexcept {
    QuicRecvExtent *extent = head_;
    while (extent != nullptr) {
        QuicRecvExtent *next = extent->next;
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
    final_size_ = 0;
    has_final_size_ = false;
}

std::uint64_t QuicStreamReassembler::block_index(std::uint64_t offset) noexcept {
    return offset / kQuicStreamRecvBlockSize;
}

std::size_t QuicStreamReassembler::block_offset(std::uint64_t offset) noexcept {
    return static_cast<std::size_t>(offset % kQuicStreamRecvBlockSize);
}

std::uint64_t QuicStreamReassembler::block_end(std::uint64_t offset) noexcept {
    const std::uint64_t block = block_index(offset);
    if (block >= std::numeric_limits<std::uint64_t>::max() / kQuicStreamRecvBlockSize) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return (block + 1) * kQuicStreamRecvBlockSize;
}

QuicRecvExtent *QuicStreamReassembler::find_prev(std::uint64_t offset) noexcept {
    QuicRecvExtent *prev = nullptr;
    QuicRecvExtent *extent = head_;
    while (extent != nullptr && extent->start <= offset) {
        prev = extent;
        extent = extent->next;
    }
    return prev;
}

common::IoResult<QuicRecvExtent *> QuicStreamReassembler::create_extent(std::uint64_t start, std::uint64_t end,
                                                                        QuicRecvExtent *prev, QuicRecvExtent *next,
                                                                        const std::uint8_t *src) noexcept {
    if (start >= end || src == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (active_extent_count_ >= kQuicStreamRecvMaxActiveExtents) {
        return std::unexpected(common::IoErr::MessageTooLarge);
    }

    const std::uint64_t block = block_index(start);
    const bool reuse_prev = prev != nullptr && prev->block_index == block;
    const bool reuse_next = next != nullptr && next->block_index == block;
    const bool new_block = !reuse_prev && !reuse_next;
    if (new_block && active_block_count_ >= kQuicStreamRecvMaxActiveBlocks) {
        return std::unexpected(common::IoErr::MessageTooLarge);
    }

    QuicRecvExtent *extent = pool_->alloc();
    if (extent == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }

    const std::size_t local = block_offset(start);
    const std::size_t len = static_cast<std::size_t>(end - start);
    mem::IoBuf view{};
    if (reuse_prev || reuse_next) {
        mem::IoBuf &owner = reuse_prev ? prev->view : next->view;
        std::memcpy(owner.data() + local, src, len);
        view = owner.retain_storage_slice(local, len);
    } else {
        mem::IoBuf storage = mem::IoBuf::allocate(kQuicStreamRecvBlockSize);
        if (!storage) {
            pool_->release(extent);
            return std::unexpected(common::IoErr::NoMem);
        }
        std::memcpy(storage.data() + local, src, len);
        view = storage.retain_storage_slice(local, len);
        ++active_block_count_;
    }

    extent->start = start;
    extent->end = end;
    extent->block_index = block;
    extent->view = std::move(view);
    extent->next = nullptr;
    ++active_extent_count_;
    buffered_bytes_ += len;
    return extent;
}

void QuicStreamReassembler::insert_after(QuicRecvExtent *prev, QuicRecvExtent &extent) noexcept {
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

QuicRecvExtent *QuicStreamReassembler::try_merge_with_next(QuicRecvExtent *extent) noexcept {
    if (extent == nullptr || extent->next == nullptr) {
        return extent;
    }

    QuicRecvExtent *right = extent->next;
    if (extent->end != right->start || extent->block_index != right->block_index ||
        !extent->view.try_merge_adjacent(std::move(right->view))) {
        return extent;
    }

    extent->end = right->end;
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

void QuicStreamReassembler::unlink_after(QuicRecvExtent *prev, QuicRecvExtent &extent) noexcept {
    const bool has_same_block = has_same_block_neighbor(prev, extent.next, extent.block_index);
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
    pool_->release(&extent);
}

bool QuicStreamReassembler::has_same_block_neighbor(const QuicRecvExtent *prev, const QuicRecvExtent *next,
                                                    std::uint64_t block) const noexcept {
    return (prev != nullptr && prev->block_index == block) || (next != nullptr && next->block_index == block);
}

} // namespace fiber::quic
