#ifndef FIBER_HTTP_HTTP3_QPACK_STATIC_TABLE_H
#define FIBER_HTTP_HTTP3_QPACK_STATIC_TABLE_H

#include <array>
#include <cstdint>
#include <string_view>

#include "Http3QpackTableEntryView.h"

namespace fiber::http {

class Http3QpackStaticTable {
public:
    static constexpr std::uint32_t kEntryCount = 99;

    using TableEntryView = Http3QpackTableEntryView;

    enum class FindKind : std::uint8_t {
        NoMatch,
        EntryMatch,
        NameMatch,
    };

    struct FindResult {
        FindKind kind = FindKind::NoMatch;
        std::uint32_t index = 0;
        TableEntryView entry{};
    };

    [[nodiscard]] static bool get_by_index(std::uint32_t index, TableEntryView &view) noexcept;
    [[nodiscard]] static FindResult find(std::string_view name, std::uint64_t name_hash,
                                         std::string_view value) noexcept;

private:
    struct StaticEntry {
        const char *name = nullptr;
        const char *value = nullptr;
        std::uint64_t name_hash = 0;
        std::uint16_t name_len = 0;
        std::uint16_t value_len = 0;
    };

    static const StaticEntry kEntries_[kEntryCount];

    // Separate-chaining hash index over kEntries_, keyed by name_hash. Built once
    // (process-lifetime magic static); find() walks only the bucket for a given
    // name_hash instead of scanning all 99 entries.
    static constexpr std::uint32_t kBucketCap = 256; // power of two; load ~0.39
    static constexpr std::uint32_t kEmptySlot = 0xffffffffU;

    struct BucketIndex {
        std::array<std::uint32_t, kBucketCap> head; // first entry id, or kEmptySlot
        std::array<std::uint32_t, kEntryCount> next; // next entry id in chain, or kEmptySlot
    };

    [[nodiscard]] static const BucketIndex &bucket_index() noexcept;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP3_QPACK_STATIC_TABLE_H
