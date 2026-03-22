#ifndef FIBER_HTTP_HTTP2_HPACK_STATIC_TABLE_H
#define FIBER_HTTP_HTTP2_HPACK_STATIC_TABLE_H

#include <cstdint>
#include <string_view>

#include "Http2HpackTableEntryView.h"

namespace fiber::http {

class Http2HpackStaticTable {
public:
    static constexpr std::uint32_t kEntryCount = 61;

    using TableEntryView = Http2HpackTableEntryView;

    [[nodiscard]] static bool get_by_index(std::uint32_t index, TableEntryView &view) noexcept;

    [[nodiscard]] static bool find_name(std::string_view name, std::uint32_t &index) noexcept;
    [[nodiscard]] static bool find_name(std::string_view lowcase_name, std::uint64_t name_hash,
                                        std::uint32_t &index) noexcept;

private:
    struct StaticEntry {
        const char *name = nullptr;
        const char *value = nullptr;
        std::uint64_t name_hash = 0;
        std::uint16_t name_len = 0;
        std::uint16_t value_len = 0;
    };

    static const StaticEntry kEntries_[kEntryCount];
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_HPACK_STATIC_TABLE_H
