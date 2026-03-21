#ifndef FIBER_HTTP_HTTP2_HPACK_DYNAMIC_TABLE_H
#define FIBER_HTTP_HTTP2_HPACK_DYNAMIC_TABLE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "Http2HpackTableEntryView.h"

namespace fiber::http {

class Http2HpackDynamicTable : public common::NonCopyable, public common::NonMovable {
public:
    using TableEntryView = Http2HpackTableEntryView;

    Http2HpackDynamicTable() noexcept = default;
    ~Http2HpackDynamicTable() = default;

    [[nodiscard]] bool init(std::uint32_t storage_cap_bytes) noexcept;
    void release() noexcept;
    void clear() noexcept;
    void set_max_size(std::uint32_t max_size) noexcept;

    [[nodiscard]] bool insert(std::string_view name, std::string_view value) noexcept;
    [[nodiscard]] bool find_exact(std::string_view name, std::string_view value,
                                  std::uint32_t &dynamic_index) const noexcept;
    // The returned views remain valid until the next mutating table operation.
    [[nodiscard]] bool get_by_index(std::uint32_t dynamic_index, TableEntryView &view) const noexcept;

    [[nodiscard]] std::uint32_t entry_count() const noexcept { return count_; }
    [[nodiscard]] std::uint32_t current_size() const noexcept { return current_size_; }
    [[nodiscard]] std::uint32_t max_size() const noexcept { return max_size_; }
    [[nodiscard]] std::uint32_t storage_capacity() const noexcept { return storage_cap_bytes_; }
    [[nodiscard]] std::uint32_t max_entries() const noexcept { return entry_cap_; }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }

private:
    static constexpr std::uint32_t kEntryOverhead = 32;
    static constexpr std::uint32_t kInvalidSlot = 0xffffffffU;

    struct DynamicEntry {
        std::uint32_t data_off = 0;
        std::uint32_t name_len = 0;
        std::uint32_t value_len = 0;
        std::uint32_t entry_size = 0;
        std::uint64_t name_hash = 0;
        std::uint64_t pair_hash = 0;
        std::uint32_t exact_prev = kInvalidSlot;
        std::uint32_t exact_next = kInvalidSlot;
        bool live = false;
    };

    [[nodiscard]] static std::uint32_t next_pow2(std::uint32_t value) noexcept;
    [[nodiscard]] std::uint32_t dynamic_index_to_slot(std::uint32_t dynamic_index) const noexcept;
    [[nodiscard]] std::uint32_t slot_to_dynamic_index(std::uint32_t slot) const noexcept;
    [[nodiscard]] std::uint32_t oldest_slot() const noexcept;
    void compact_bytes() noexcept;
    void evict_oldest() noexcept;
    void unlink_exact(std::uint32_t slot) noexcept;
    void link_exact(std::uint32_t slot) noexcept;

    std::unique_ptr<DynamicEntry[]> entries_;
    std::unique_ptr<char[]> bytes_;
    std::unique_ptr<std::uint32_t[]> exact_bucket_head_;
    std::uint32_t entry_cap_ = 0;
    std::uint32_t bytes_cap_ = 0;
    std::uint32_t exact_bucket_cap_ = 0;
    std::uint32_t storage_cap_bytes_ = 0;
    std::uint32_t max_size_ = 0;
    std::uint32_t current_size_ = 0;
    std::uint32_t count_ = 0;
    std::uint32_t head_ = 0;
    std::uint32_t bytes_begin_ = 0;
    std::uint32_t bytes_end_ = 0;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_HPACK_DYNAMIC_TABLE_H
