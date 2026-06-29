#ifndef FIBER_HTTP_HTTP3_QPACK_STATIC_TABLE_H
#define FIBER_HTTP_HTTP3_QPACK_STATIC_TABLE_H

#include <cstdint>

#include "Http3QpackTableEntryView.h"

namespace fiber::http {

class Http3QpackStaticTable {
public:
    static constexpr std::uint32_t kEntryCount = 99;

    using TableEntryView = Http3QpackTableEntryView;

    [[nodiscard]] static bool get_by_index(std::uint32_t index, TableEntryView &view) noexcept;

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

#endif // FIBER_HTTP_HTTP3_QPACK_STATIC_TABLE_H
