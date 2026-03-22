#include "Http2HpackStaticTable.h"

#include <cstring>

#include "HttpHeaderHash.h"

namespace fiber::http {

namespace {

#define FIBER_HTTP2_STATIC_ENTRY(name_literal, value_literal)                                                \
    {                                                                                                         \
        name_literal, value_literal, fiber::http::http_header_name_hash(name_literal),                       \
            static_cast<std::uint16_t>(sizeof(name_literal) - 1),                                             \
            static_cast<std::uint16_t>(sizeof(value_literal) - 1)                                             \
    }

inline bool same_bytes(std::string_view left, const char *right, std::size_t right_len) noexcept {
    return left.size() == right_len && std::memcmp(left.data(), right, right_len) == 0;
}

} // namespace

const Http2HpackStaticTable::StaticEntry Http2HpackStaticTable::kEntries_[kEntryCount] = {
    FIBER_HTTP2_STATIC_ENTRY(":authority", ""),
    FIBER_HTTP2_STATIC_ENTRY(":method", "GET"),
    FIBER_HTTP2_STATIC_ENTRY(":method", "POST"),
    FIBER_HTTP2_STATIC_ENTRY(":path", "/"),
    FIBER_HTTP2_STATIC_ENTRY(":path", "/index.html"),
    FIBER_HTTP2_STATIC_ENTRY(":scheme", "http"),
    FIBER_HTTP2_STATIC_ENTRY(":scheme", "https"),
    FIBER_HTTP2_STATIC_ENTRY(":status", "200"),
    FIBER_HTTP2_STATIC_ENTRY(":status", "204"),
    FIBER_HTTP2_STATIC_ENTRY(":status", "206"),
    FIBER_HTTP2_STATIC_ENTRY(":status", "304"),
    FIBER_HTTP2_STATIC_ENTRY(":status", "400"),
    FIBER_HTTP2_STATIC_ENTRY(":status", "404"),
    FIBER_HTTP2_STATIC_ENTRY(":status", "500"),
    FIBER_HTTP2_STATIC_ENTRY("accept-charset", ""),
    FIBER_HTTP2_STATIC_ENTRY("accept-encoding", "gzip, deflate"),
    FIBER_HTTP2_STATIC_ENTRY("accept-language", ""),
    FIBER_HTTP2_STATIC_ENTRY("accept-ranges", ""),
    FIBER_HTTP2_STATIC_ENTRY("accept", ""),
    FIBER_HTTP2_STATIC_ENTRY("access-control-allow-origin", ""),
    FIBER_HTTP2_STATIC_ENTRY("age", ""),
    FIBER_HTTP2_STATIC_ENTRY("allow", ""),
    FIBER_HTTP2_STATIC_ENTRY("authorization", ""),
    FIBER_HTTP2_STATIC_ENTRY("cache-control", ""),
    FIBER_HTTP2_STATIC_ENTRY("content-disposition", ""),
    FIBER_HTTP2_STATIC_ENTRY("content-encoding", ""),
    FIBER_HTTP2_STATIC_ENTRY("content-language", ""),
    FIBER_HTTP2_STATIC_ENTRY("content-length", ""),
    FIBER_HTTP2_STATIC_ENTRY("content-location", ""),
    FIBER_HTTP2_STATIC_ENTRY("content-range", ""),
    FIBER_HTTP2_STATIC_ENTRY("content-type", ""),
    FIBER_HTTP2_STATIC_ENTRY("cookie", ""),
    FIBER_HTTP2_STATIC_ENTRY("date", ""),
    FIBER_HTTP2_STATIC_ENTRY("etag", ""),
    FIBER_HTTP2_STATIC_ENTRY("expect", ""),
    FIBER_HTTP2_STATIC_ENTRY("expires", ""),
    FIBER_HTTP2_STATIC_ENTRY("from", ""),
    FIBER_HTTP2_STATIC_ENTRY("host", ""),
    FIBER_HTTP2_STATIC_ENTRY("if-match", ""),
    FIBER_HTTP2_STATIC_ENTRY("if-modified-since", ""),
    FIBER_HTTP2_STATIC_ENTRY("if-none-match", ""),
    FIBER_HTTP2_STATIC_ENTRY("if-range", ""),
    FIBER_HTTP2_STATIC_ENTRY("if-unmodified-since", ""),
    FIBER_HTTP2_STATIC_ENTRY("last-modified", ""),
    FIBER_HTTP2_STATIC_ENTRY("link", ""),
    FIBER_HTTP2_STATIC_ENTRY("location", ""),
    FIBER_HTTP2_STATIC_ENTRY("max-forwards", ""),
    FIBER_HTTP2_STATIC_ENTRY("proxy-authenticate", ""),
    FIBER_HTTP2_STATIC_ENTRY("proxy-authorization", ""),
    FIBER_HTTP2_STATIC_ENTRY("range", ""),
    FIBER_HTTP2_STATIC_ENTRY("referer", ""),
    FIBER_HTTP2_STATIC_ENTRY("refresh", ""),
    FIBER_HTTP2_STATIC_ENTRY("retry-after", ""),
    FIBER_HTTP2_STATIC_ENTRY("server", ""),
    FIBER_HTTP2_STATIC_ENTRY("set-cookie", ""),
    FIBER_HTTP2_STATIC_ENTRY("strict-transport-security", ""),
    FIBER_HTTP2_STATIC_ENTRY("transfer-encoding", ""),
    FIBER_HTTP2_STATIC_ENTRY("user-agent", ""),
    FIBER_HTTP2_STATIC_ENTRY("vary", ""),
    FIBER_HTTP2_STATIC_ENTRY("via", ""),
    FIBER_HTTP2_STATIC_ENTRY("www-authenticate", ""),
};

bool Http2HpackStaticTable::get_by_index(std::uint32_t index, TableEntryView &view) noexcept {
    view = TableEntryView{};
    if (index == 0 || index > kEntryCount) {
        return false;
    }

    const StaticEntry &entry = kEntries_[index - 1];
    view.name = std::string_view(entry.name, entry.name_len);
    view.value = std::string_view(entry.value, entry.value_len);
    view.name_hash = entry.name_hash;
    return true;
}

bool Http2HpackStaticTable::find_name(std::string_view name, std::uint32_t &index) noexcept {
    const std::uint64_t name_hash = http_header_name_hash(name);
    for (std::uint32_t i = 0; i < kEntryCount; ++i) {
        const StaticEntry &entry = kEntries_[i];
        if (entry.name_hash != name_hash || entry.name_len != name.size()) {
            continue;
        }
        if (http_header_name_equals_ci(name, std::string_view(entry.name, entry.name_len))) {
            index = i + 1;
            return true;
        }
    }
    return false;
}

bool Http2HpackStaticTable::find_name(std::string_view lowcase_name, std::uint64_t name_hash,
                                      std::uint32_t &index) noexcept {
    for (std::uint32_t i = 0; i < kEntryCount; ++i) {
        const StaticEntry &entry = kEntries_[i];
        if (entry.name_hash != name_hash || entry.name_len != lowcase_name.size()) {
            continue;
        }
        if (same_bytes(lowcase_name, entry.name, entry.name_len)) {
            index = i + 1;
            return true;
        }
    }
    return false;
}

#undef FIBER_HTTP2_STATIC_ENTRY

} // namespace fiber::http
