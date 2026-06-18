#include "QuicDataReassembler.h"

#include <algorithm>
#include <cstring>
#include <expected>
#include <limits>

#include "../common/Assert.h"

namespace fiber::quic {

namespace {

static constexpr std::size_t kQuicCryptoRecvMaxActiveExtents = 1024;
static constexpr unsigned kBlockSizeShift = 12;
static constexpr std::uint64_t kBlockOffsetMask = (1ULL << kBlockSizeShift) - 1;

} // namespace

QuicDataReassembler::~QuicDataReassembler() { clear(); }

void QuicDataReassembler::init(mem::IoBufNodePool &pool) noexcept { init(pool, Options{}); }

void QuicDataReassembler::init(mem::IoBufNodePool &pool, Options options) noexcept {
    FIBER_ASSERT(pool_ == nullptr || pool_ == &pool);
    clear();
    pool_ = &pool;
    buffer_limit_ = options.buffer_limit;
}

void QuicDataReassembler::clear() noexcept {
    while (head_ != nullptr) {
        mem::IoBufNode *next = head_->next;
        pool_->release(head_);
        head_ = next;
    }
    head_ = nullptr;
    tail_ = nullptr;
    last_insert_ = nullptr;
    buffered_bytes_ = 0;
    active_extent_count_ = 0;
    active_block_count_ = 0;
    next_offset_ = 0;
}

common::IoResult<std::size_t> QuicDataReassembler::insert(std::uint64_t offset, QuicSlice data) noexcept {
    FIBER_ASSERT(pool_ != nullptr);

    if ((data.data == nullptr && data.len != 0) || offset > std::numeric_limits<std::uint64_t>::max() - data.len)
            [[unlikely]] {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::uint64_t data_end = offset + data.len;
    if (data_end <= next_offset_ || data.len == 0) {
        return 0;
    }

    if (offset < next_offset_) {
        const auto skip = static_cast<std::size_t>(next_offset_ - offset);
        data.data += skip;
        data.len -= skip;
        offset = next_offset_;
    }

    auto cost = insert_cost(offset, data.len);
    if (!cost) [[unlikely]] {
        return std::unexpected(cost.error());
    }
    std::size_t immediately_takeable_bytes = 0;
    const bool starts_at_next_offset = offset == next_offset_;
    if (starts_at_next_offset) {
        const std::uint64_t contiguous_end = contiguous_end_after_insert(offset, data.len);
        if (contiguous_end > offset) {
            auto takeable_cost = insert_cost(offset, static_cast<std::size_t>(contiguous_end - offset));
            if (!takeable_cost) [[unlikely]] {
                return std::unexpected(takeable_cost.error());
            }
            immediately_takeable_bytes = takeable_cost->bytes;
        }
    }

    const std::size_t active_block_limit =
            std::max<std::size_t>(1, (buffer_limit_ + kRecvBlockSize - 1) / kRecvBlockSize);
    if (buffered_bytes_ + cost->bytes - immediately_takeable_bytes > buffer_limit_ ||
        (!starts_at_next_offset && (active_extent_count_ + cost->extents > kQuicCryptoRecvMaxActiveExtents ||
                                    active_block_count_ + cost->blocks > active_block_limit))) [[unlikely]] {
        return std::unexpected(common::IoErr::MessageTooLarge);
    }

    const std::uint64_t base_offset = offset;
    std::uint64_t cursor = offset;
    std::size_t copied = 0;
    mem::IoBufNode *prev = nullptr;
    mem::IoBufNode *cur = head_;

    if (last_insert_ != nullptr && last_insert_->offset <= cursor) {
        prev = last_insert_;
        cur = last_insert_->next;
    }

    while (cursor < data_end) {
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

        if (prev != nullptr && block_of(prev->offset) == block) {
            (void) try_merge_with_next(prev);
        }

        if (prev != nullptr && prev->next != new_ext) {
            (void) try_merge_with_next(prev);
        } else {
            (void) try_merge_with_next(new_ext);
            prev = new_ext;
        }

        cursor = hole_end;
        copied += hole_len;
        cur = prev->next;
        last_insert_ = prev;
    }

    return copied;
}

common::IoResult<std::size_t> QuicDataReassembler::take_contiguous(mem::IoBufChain &out) noexcept {
    FIBER_ASSERT(pool_ != nullptr);
    FIBER_ASSERT(&out.node_pool() == pool_);

    std::size_t taken = 0;
    while (head_ != nullptr && head_->offset == next_offset_) {
        mem::IoBufNode *extent = head_;
        const std::size_t readable = extent->buf.readable();
        const std::uint64_t next_read = extent->offset + readable;
        buffered_bytes_ -= readable;
        taken += readable;
        unlink_after(nullptr, *extent);
        next_offset_ = next_read;
        if (!out.append_node(extent)) {
            return std::unexpected(common::IoErr::NoMem);
        }
    }

    return taken;
}

common::IoResult<QuicDataReassembler::InsertCost> QuicDataReassembler::insert_cost(std::uint64_t offset,
                                                                                   std::size_t len) const noexcept {
    if (offset > std::numeric_limits<std::uint64_t>::max() - len) [[unlikely]] {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::uint64_t data_end = offset + len;
    if (data_end <= next_offset_ || len == 0) {
        return InsertCost{};
    }

    if (offset < next_offset_) {
        offset = next_offset_;
    }

    InsertCost cost{};
    std::uint64_t cursor = offset;
    mem::IoBufNode *prev = nullptr;
    mem::IoBufNode *cur = head_;

    if (last_insert_ != nullptr && last_insert_->offset <= cursor) {
        prev = last_insert_;
        cur = last_insert_->next;
    }

    while (cursor < data_end) {
        while (cur != nullptr && cur->offset <= cursor) {
            const std::uint64_t cur_end = cur->offset + cur->buf.readable();
            if (cur_end > cursor) {
                cursor = std::min(cur_end, data_end);
                if (cursor >= data_end) {
                    return cost;
                }
            }
            prev = cur;
            cur = cur->next;
        }

        if (prev != nullptr) {
            const std::uint64_t prev_end = prev->offset + prev->buf.readable();
            if (prev_end > cursor) {
                cursor = std::min(prev_end, data_end);
                if (cursor >= data_end) {
                    return cost;
                }
                continue;
            }
        }

        const std::uint64_t next_start = cur != nullptr ? cur->offset : data_end;
        const std::uint64_t hole_end = std::min({next_start, data_end, block_end(cursor)});
        if (hole_end <= cursor) [[unlikely]] {
            return std::unexpected(common::IoErr::Invalid);
        }

        const std::uint64_t block = block_of(cursor);
        cost.bytes += static_cast<std::size_t>(hole_end - cursor);
        ++cost.extents;
        if (!has_same_block_neighbor(prev, cur, block)) {
            ++cost.blocks;
        }
        cursor = hole_end;
    }

    return cost;
}

std::uint64_t QuicDataReassembler::contiguous_end_after_insert(std::uint64_t offset, std::size_t len) const noexcept {
    const std::uint64_t data_end = offset + len;
    if (offset != next_offset_ || data_end <= next_offset_) {
        return next_offset_;
    }

    std::uint64_t cursor = data_end;
    bool progressed = true;
    while (progressed) {
        progressed = false;
        for (mem::IoBufNode *node = head_; node != nullptr; node = node->next) {
            if (node->offset > cursor) {
                break;
            }
            const std::uint64_t node_end = node->offset + node->buf.readable();
            if (node_end > cursor) {
                cursor = node_end;
                progressed = true;
            }
        }
    }
    return cursor;
}

std::uint64_t QuicDataReassembler::block_of(std::uint64_t offset) noexcept { return offset >> kBlockSizeShift; }

std::size_t QuicDataReassembler::block_offset(std::uint64_t offset) noexcept {
    return static_cast<std::size_t>(offset & kBlockOffsetMask);
}

std::uint64_t QuicDataReassembler::block_end(std::uint64_t offset) noexcept {
    return (offset & ~kBlockOffsetMask) + kRecvBlockSize;
}

common::IoResult<mem::IoBufNode *> QuicDataReassembler::create_extent(std::uint64_t offset, std::size_t len,
                                                                      mem::IoBufNode *prev, mem::IoBufNode *next,
                                                                      const std::uint8_t *src,
                                                                      std::uint64_t block) noexcept {
    if (len == 0 || src == nullptr) [[unlikely]] {
        return std::unexpected(common::IoErr::Invalid);
    }

    const bool reuse_prev = prev != nullptr && block_of(prev->offset) == block;
    const bool reuse_next = next != nullptr && block_of(next->offset) == block;
    const bool new_block = !reuse_prev && !reuse_next;

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
        mem::IoBuf storage = mem::IoBuf::allocate(kRecvBlockSize);
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

void QuicDataReassembler::insert_after(mem::IoBufNode *prev, mem::IoBufNode &extent) noexcept {
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

mem::IoBufNode *QuicDataReassembler::try_merge_with_next(mem::IoBufNode *extent) noexcept {
    if (extent == nullptr || extent->next == nullptr) {
        return extent;
    }

    mem::IoBufNode *right = extent->next;
    if (block_of(extent->offset) != block_of(right->offset) || !extent->buf.try_merge_adjacent(std::move(right->buf))) {
        return extent;
    }

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

void QuicDataReassembler::unlink_after(mem::IoBufNode *prev, mem::IoBufNode &extent) noexcept {
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

bool QuicDataReassembler::has_same_block_neighbor(const mem::IoBufNode *prev, const mem::IoBufNode *next,
                                                  std::uint64_t block) noexcept {
    return (prev != nullptr && block_of(prev->offset) == block) || (next != nullptr && block_of(next->offset) == block);
}

} // namespace fiber::quic
