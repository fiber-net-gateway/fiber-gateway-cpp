#ifndef FIBER_HTTP_HTTP2_HPACK_ENCODE_CATALOG_H
#define FIBER_HTTP_HTTP2_HPACK_ENCODE_CATALOG_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "Http2HpackStaticTable.h"

namespace fiber::http {

class Http2HpackEncodeCatalog : public common::NonCopyable, public common::NonMovable {
public:
    struct PolicyEntry {
        std::string_view name;
        std::uint64_t name_hash = 0;
        std::string_view value;
    };

    enum class EntryKind : std::uint8_t {
        Static,
        Policy,
    };

    struct EntryView {
        EntryKind kind = EntryKind::Static;
        std::uint32_t catalog_id = 0;
        std::uint32_t policy_slot = 0;
        std::string_view name;
        std::uint64_t name_hash = 0;
        std::string_view value;
        std::uint32_t hpack_index = 0;
        std::uint32_t entry_size = 0;
    };

    struct FindResult {
        // Preferred entry for referencing the field name, regardless of value.
        const EntryView *name_entry = nullptr;
        // Preferred entry whose name and value both match.
        const EntryView *exact_entry = nullptr;
    };

    Http2HpackEncodeCatalog() noexcept = default;

    [[nodiscard]] bool init(std::span<const PolicyEntry> policy_entries) noexcept;
    void release() noexcept;

    [[nodiscard]] std::uint32_t size() const noexcept { return entry_count_; }
    [[nodiscard]] std::uint32_t static_count() const noexcept { return Http2HpackStaticTable::kEntryCount; }
    [[nodiscard]] std::uint32_t policy_count() const noexcept { return policy_count_; }
    [[nodiscard]] bool empty() const noexcept { return entry_count_ == 0; }

    [[nodiscard]] bool is_static_entry(const EntryView *entry) const noexcept;
    [[nodiscard]] bool is_policy_entry(const EntryView *entry) const noexcept;
    [[nodiscard]] FindResult find(std::string_view name, std::uint64_t name_hash,
                                  std::string_view value) const noexcept;

private:
    static constexpr std::uint32_t kInvalidId = 0xffffffffU;

    [[nodiscard]] static std::uint32_t next_pow2(std::uint32_t value) noexcept;
    [[nodiscard]] static bool prefer_entry(const EntryView &candidate, const EntryView &current) noexcept;

    std::unique_ptr<EntryView[]> entries_;
    std::unique_ptr<std::uint32_t[]> bucket_head_;
    std::unique_ptr<std::uint32_t[]> next_bucket_;
    std::uint32_t entry_count_ = 0;
    std::uint32_t policy_count_ = 0;
    std::uint32_t bucket_cap_ = 0;

    friend class Http2HpackEncodeTable;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_HPACK_ENCODE_CATALOG_H
