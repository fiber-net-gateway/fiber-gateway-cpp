#ifndef FIBER_HTTP_HTTP3_QPACK_TABLE_ENTRY_VIEW_H
#define FIBER_HTTP_HTTP3_QPACK_TABLE_ENTRY_VIEW_H

#include <cstdint>
#include <string_view>

namespace fiber::http {

struct Http3QpackTableEntryView {
    std::string_view name;
    std::string_view value;
    std::uint64_t name_hash = 0;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP3_QPACK_TABLE_ENTRY_VIEW_H
