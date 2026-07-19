#include "QuicDataReassembler.h"

#include <algorithm>
#include <cstring>
#include <expected>
#include <limits>
#include <utility>

#include "../common/Assert.h"

namespace fiber::quic {

QuicDataReassembler::QuicDataReassembler(mem::IoBufNodePool &pool) noexcept : QuicDataReassembler(pool, Options{}) {}

QuicDataReassembler::QuicDataReassembler(mem::IoBufNodePool &pool, Options options) noexcept { init(pool, options); }

QuicDataReassembler::~QuicDataReassembler() { discard_buffered(); }

void QuicDataReassembler::init(mem::IoBufNodePool &pool) noexcept { init(pool, Options{}); }

void QuicDataReassembler::init(mem::IoBufNodePool &pool, Options options) noexcept {
    FIBER_ASSERT(pool_ == nullptr || pool_ == &pool);
    clear();
    pool_ = &pool;
    buffer_limit_ = options.buffer_limit;
    max_active_extents_ = options.max_active_extents;
    buffer_accounting_ = options.buffer_accounting;
    storage_budget_ = options.storage_budget;
    compact_min_backing_capacity_ = options.compact_min_backing_capacity;
    compact_ratio_ = std::max<std::uint8_t>(options.compact_ratio, 1);
}

void QuicDataReassembler::discard_buffered() noexcept {
    if (pool_ == nullptr) {
        return;
    }
    while (head_ != nullptr) {
        mem::IoBufNode *next = head_->next;
        if (storage_budget_ != nullptr) {
            storage_budget_->release(head_->buf);
        }
        pool_->release(head_);
        head_ = next;
    }
    tail_ = nullptr;
    last_insert_ = nullptr;
    buffered_bytes_ = 0;
    active_extent_count_ = 0;
}

void QuicDataReassembler::clear() noexcept {
    discard_buffered();
    next_offset_ = 0;
}

common::IoResult<std::size_t> QuicDataReassembler::insert(std::uint64_t offset, mem::IoBuf data) noexcept {
    FIBER_ASSERT(pool_ != nullptr);

    const std::size_t data_len = data.readable();
    if ((data_len != 0 && !data) || offset > std::numeric_limits<std::uint64_t>::max() - data_len) [[unlikely]] {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::uint64_t original_end = offset + data_len;
    if (original_end <= next_offset_ || data_len == 0) {
        return 0;
    }

    if (offset < next_offset_) {
        const auto skip = static_cast<std::size_t>(next_offset_ - offset);
        data.consume(skip);
        offset = next_offset_;
    }

    const std::size_t retained_len = data.readable();
    const std::uint64_t data_end = offset + retained_len;
    auto cost = insert_cost(offset, retained_len);
    if (!cost) [[unlikely]] {
        return std::unexpected(cost.error());
    }

    std::size_t immediately_takeable_bytes = 0;
    const bool starts_at_next_offset = offset == next_offset_;
    if (buffer_accounting_ == BufferAccounting::OutOfOrderOnly && starts_at_next_offset) {
        const std::uint64_t contiguous_end = contiguous_end_after_insert(offset, retained_len);
        if (contiguous_end > offset) {
            auto takeable_cost = insert_cost(offset, static_cast<std::size_t>(contiguous_end - offset));
            if (!takeable_cost) [[unlikely]] {
                return std::unexpected(takeable_cost.error());
            }
            immediately_takeable_bytes = takeable_cost->bytes;
        }
    }

    if (cost->bytes > std::numeric_limits<std::size_t>::max() - buffered_bytes_) [[unlikely]] {
        return std::unexpected(common::IoErr::MessageTooLarge);
    }
    const std::size_t retained_after_insert = buffered_bytes_ + cost->bytes;
    if (immediately_takeable_bytes > retained_after_insert ||
        retained_after_insert - immediately_takeable_bytes > buffer_limit_) [[unlikely]] {
        return std::unexpected(common::IoErr::MessageTooLarge);
    }
    if ((!starts_at_next_offset || buffer_accounting_ == BufferAccounting::AllRetained) &&
        cost->extents > max_active_extents_ - std::min(active_extent_count_, max_active_extents_)) [[unlikely]] {
        return std::unexpected(common::IoErr::MessageTooLarge);
    }
    if (cost->extents == 0) {
        return 0;
    }

    mem::IoBuf compacted;
    bool use_compacted = false;
    if (storage_budget_ != nullptr) {
        const bool amplified = !storage_budget_->retains(data) && data.capacity() >= compact_min_backing_capacity_ &&
                               cost->bytes <= data.capacity() / compact_ratio_;
        if (!storage_budget_->compatible(data) || amplified) {
            auto compact = compact_missing(offset, data, cost->bytes);
            if (!compact) [[unlikely]] {
                return std::unexpected(compact.error());
            }
            compacted = std::move(*compact);
            use_compacted = true;
        }
    }

    mem::IoBufNode *reserved = reserve_nodes(cost->extents);
    if (reserved == nullptr) [[unlikely]] {
        return std::unexpected(common::IoErr::NoMem);
    }

    if (storage_budget_ != nullptr) {
        mem::IoBuf *backing = use_compacted ? &compacted : &data;
        const auto refs = static_cast<std::uint32_t>(cost->extents);
        bool retained_storage = storage_budget_->try_retain(*backing, refs);
        if (!retained_storage && !use_compacted && cost->bytes < data.capacity()) [[unlikely]] {
            auto compact = compact_missing(offset, data, cost->bytes);
            if (compact) {
                compacted = std::move(*compact);
                use_compacted = true;
                backing = &compacted;
                retained_storage = storage_budget_->try_retain(*backing, refs);
            }
        }
        if (!retained_storage) [[unlikely]] {
            release_nodes(reserved);
            return std::unexpected(common::IoErr::NoMem);
        }
    }

    const std::uint64_t base_offset = offset;
    std::uint64_t cursor = offset;
    std::size_t retained = 0;
    std::size_t compacted_offset = 0;
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
                    break;
                }
            }
            prev = cur;
            cur = cur->next;
        }
        if (cursor >= data_end) {
            break;
        }

        if (prev != nullptr) {
            const std::uint64_t prev_end = prev->offset + prev->buf.readable();
            if (prev_end > cursor) {
                cursor = std::min(prev_end, data_end);
                if (cursor >= data_end) {
                    break;
                }
                continue;
            }
        }

        const std::uint64_t next_start = cur != nullptr ? cur->offset : data_end;
        const std::uint64_t hole_end = std::min(next_start, data_end);
        FIBER_ASSERT(hole_end > cursor);
        FIBER_ASSERT(reserved != nullptr);

        mem::IoBufNode *new_ext = reserved;
        reserved = reserved->next;
        new_ext->next = nullptr;
        new_ext->offset = cursor;

        const std::size_t source_offset = static_cast<std::size_t>(cursor - base_offset);
        const std::size_t hole_len = static_cast<std::size_t>(hole_end - cursor);
        if (use_compacted && cost->extents == 1) {
            new_ext->buf = std::move(compacted);
        } else if (use_compacted) {
            new_ext->buf = compacted.retain_slice(compacted_offset, hole_len);
            compacted_offset += hole_len;
        } else if (cost->extents == 1 && source_offset == 0 && hole_len == data.readable()) {
            new_ext->buf = std::move(data);
        } else {
            new_ext->buf = data.retain_slice(source_offset, hole_len);
        }

        insert_after(prev, *new_ext);
        ++active_extent_count_;
        buffered_bytes_ += hole_len;
        retained += hole_len;

        if (prev != nullptr && prev->offset + prev->buf.readable() == new_ext->offset) {
            prev = try_merge_with_next(prev);
        } else {
            prev = new_ext;
        }
        prev = try_merge_with_next(prev);
        cur = prev->next;
        cursor = hole_end;
        last_insert_ = prev;
    }

    FIBER_ASSERT(reserved == nullptr);
    return retained;
}

common::IoResult<std::size_t> QuicDataReassembler::take_contiguous(mem::IoBufChain &out,
                                                                   std::size_t max_bytes) noexcept {
    FIBER_ASSERT(pool_ != nullptr);
    if (!out.bound()) {
        out.bind_node_pool(*pool_);
    }
    FIBER_ASSERT(&out.node_pool() == pool_);

    std::size_t taken = 0;
    while (max_bytes != 0 && head_ != nullptr && head_->offset == next_offset_) {
        mem::IoBufNode *extent = head_;
        const std::size_t readable = extent->buf.readable();
        const std::size_t take_bytes = std::min(readable, max_bytes);

        if (take_bytes == readable) [[likely]] {
            const std::uint64_t next_read = extent->offset + readable;
            buffered_bytes_ -= readable;
            max_bytes -= readable;
            taken += readable;
            unlink_after(nullptr, *extent);
            next_offset_ = next_read;
            if (storage_budget_ != nullptr) {
                storage_budget_->release(extent->buf);
            }
            if (!out.append_node(extent)) [[unlikely]] {
                return std::unexpected(common::IoErr::NoMem);
            }
            continue;
        }

        mem::IoBuf piece = extent->buf.retain_slice(0, take_bytes);
        if (!piece || !out.append(std::move(piece))) [[unlikely]] {
            return std::unexpected(common::IoErr::NoMem);
        }
        extent->buf.consume(take_bytes);
        extent->offset += take_bytes;
        next_offset_ += take_bytes;
        buffered_bytes_ -= take_bytes;
        taken += take_bytes;
        break;
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
    offset = std::max(offset, next_offset_);

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
        const std::uint64_t hole_end = std::min(next_start, data_end);
        if (hole_end <= cursor) [[unlikely]] {
            return std::unexpected(common::IoErr::Invalid);
        }
        cost.bytes += static_cast<std::size_t>(hole_end - cursor);
        ++cost.extents;
        cursor = hole_end;
    }

    return cost;
}

common::IoResult<mem::IoBuf> QuicDataReassembler::compact_missing(std::uint64_t offset, const mem::IoBuf &data,
                                                                  std::size_t retained_bytes) const noexcept {
    if (retained_bytes == 0 || offset > std::numeric_limits<std::uint64_t>::max() - data.readable()) [[unlikely]] {
        return std::unexpected(common::IoErr::Invalid);
    }

    mem::IoBuf compacted = mem::IoBuf::allocate_trackable(retained_bytes);
    if (!compacted) [[unlikely]] {
        return std::unexpected(common::IoErr::NoMem);
    }

    const std::uint64_t base_offset = offset;
    const std::uint64_t data_end = offset + data.readable();
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
                    break;
                }
            }
            prev = cur;
            cur = cur->next;
        }
        if (cursor >= data_end) {
            break;
        }

        if (prev != nullptr) {
            const std::uint64_t prev_end = prev->offset + prev->buf.readable();
            if (prev_end > cursor) {
                cursor = std::min(prev_end, data_end);
                continue;
            }
        }

        const std::uint64_t next_start = cur != nullptr ? cur->offset : data_end;
        const std::uint64_t hole_end = std::min(next_start, data_end);
        if (hole_end <= cursor) [[unlikely]] {
            return std::unexpected(common::IoErr::Invalid);
        }

        const std::size_t source_offset = static_cast<std::size_t>(cursor - base_offset);
        const std::size_t hole_len = static_cast<std::size_t>(hole_end - cursor);
        std::memcpy(compacted.writable_data(), data.readable_data() + source_offset, hole_len);
        compacted.commit(hole_len);
        cursor = hole_end;
    }

    FIBER_ASSERT(compacted.readable() == retained_bytes);
    return compacted;
}

std::uint64_t QuicDataReassembler::contiguous_end_after_insert(std::uint64_t offset, std::size_t len) const noexcept {
    const std::uint64_t data_end = offset + len;
    if (offset != next_offset_ || data_end <= next_offset_) {
        return next_offset_;
    }

    std::uint64_t cursor = data_end;
    for (mem::IoBufNode *node = head_; node != nullptr && node->offset <= cursor; node = node->next) {
        const std::uint64_t node_end = node->offset + node->buf.readable();
        cursor = std::max(cursor, node_end);
    }
    return cursor;
}

mem::IoBufNode *QuicDataReassembler::reserve_nodes(std::size_t count) noexcept {
    mem::IoBufNode *nodes = nullptr;
    for (std::size_t i = 0; i < count; ++i) {
        mem::IoBufNode *node = pool_->alloc();
        if (node == nullptr) [[unlikely]] {
            release_nodes(nodes);
            return nullptr;
        }
        node->next = nodes;
        nodes = node;
    }
    return nodes;
}

void QuicDataReassembler::release_nodes(mem::IoBufNode *nodes) noexcept {
    while (nodes != nullptr) {
        mem::IoBufNode *next = nodes->next;
        pool_->release(nodes);
        nodes = next;
    }
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
    if (extent->offset + extent->buf.readable() != right->offset ||
        !extent->buf.try_merge_adjacent(std::move(right->buf))) {
        return extent;
    }

    extent->next = right->next;
    if (tail_ == right) {
        tail_ = extent;
    }
    if (last_insert_ == right) {
        last_insert_ = extent;
    }
    if (storage_budget_ != nullptr) {
        storage_budget_->release(extent->buf);
    }
    --active_extent_count_;
    pool_->release(right);
    return extent;
}

void QuicDataReassembler::unlink_after(mem::IoBufNode *prev, mem::IoBufNode &extent) noexcept {
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
    --active_extent_count_;
}

} // namespace fiber::quic
