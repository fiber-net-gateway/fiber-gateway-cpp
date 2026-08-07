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

    struct FindResult {
        std::uint8_t name_index = 0;
        std::uint8_t exact_index = 0;
    };

    [[nodiscard]] static bool get_by_index(std::uint32_t index, TableEntryView &view) noexcept;

    [[nodiscard]] static FindResult find(std::string_view name, std::uint64_t name_hash,
                                         std::string_view value) noexcept;

    [[nodiscard]] static bool find_name(std::string_view name, std::uint32_t &index) noexcept;
    [[nodiscard]] static bool find_name(std::string_view name, std::uint64_t name_hash, std::uint32_t &index) noexcept;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_HPACK_STATIC_TABLE_H
