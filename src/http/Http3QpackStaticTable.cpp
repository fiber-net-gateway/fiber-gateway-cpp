#include "Http3QpackStaticTable.h"

#include <string_view>

#include "HttpHeaderHash.h"

namespace fiber::http {

namespace {

#define FIBER_HTTP3_QPACK_STATIC_ENTRY(name_literal, value_literal)                                                    \
    {name_literal, value_literal, fiber::http::http_header_name_hash(name_literal),                                    \
     static_cast<std::uint16_t>(sizeof(name_literal) - 1), static_cast<std::uint16_t>(sizeof(value_literal) - 1)}

[[nodiscard]] bool same_bytes(std::string_view left, std::string_view right) noexcept {
    return left.size() == right.size() && left == right;
}

} // namespace

const Http3QpackStaticTable::StaticEntry Http3QpackStaticTable::kEntries_[kEntryCount] = {
        FIBER_HTTP3_QPACK_STATIC_ENTRY(":authority", ""),
        FIBER_HTTP3_QPACK_STATIC_ENTRY(":path", "/"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("age", "0"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("content-disposition", ""),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("content-length", "0"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("cookie", ""),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("date", ""),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("etag", ""),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("if-modified-since", ""),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("if-none-match", ""),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("last-modified", ""),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("link", ""),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("location", ""),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("referer", ""),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("set-cookie", ""),
        FIBER_HTTP3_QPACK_STATIC_ENTRY(":method", "CONNECT"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY(":method", "DELETE"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY(":method", "GET"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY(":method", "HEAD"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY(":method", "OPTIONS"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY(":method", "POST"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY(":method", "PUT"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY(":scheme", "http"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY(":scheme", "https"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY(":status", "103"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY(":status", "200"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY(":status", "304"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY(":status", "404"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY(":status", "503"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("accept", "*/*"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("accept", "application/dns-message"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("accept-encoding", "gzip, deflate, br"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("accept-ranges", "bytes"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("access-control-allow-headers", "cache-control"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("access-control-allow-headers", "content-type"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("access-control-allow-origin", "*"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("cache-control", "max-age=0"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("cache-control", "max-age=2592000"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("cache-control", "max-age=604800"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("cache-control", "no-cache"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("cache-control", "no-store"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("cache-control", "public, max-age=31536000"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("content-encoding", "br"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("content-encoding", "gzip"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("content-type", "application/dns-message"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("content-type", "application/javascript"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("content-type", "application/json"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("content-type", "application/x-www-form-urlencoded"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("content-type", "image/gif"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("content-type", "image/jpeg"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("content-type", "image/png"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("content-type", "text/css"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("content-type", "text/html; charset=utf-8"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("content-type", "text/plain"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("content-type", "text/plain;charset=utf-8"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("range", "bytes=0-"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("strict-transport-security", "max-age=31536000"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("strict-transport-security", "max-age=31536000; includesubdomains"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("strict-transport-security", "max-age=31536000; includesubdomains; preload"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("vary", "accept-encoding"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("vary", "origin"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("x-content-type-options", "nosniff"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("x-xss-protection", "1; mode=block"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY(":status", "100"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY(":status", "204"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY(":status", "206"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY(":status", "302"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY(":status", "400"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY(":status", "403"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY(":status", "421"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY(":status", "425"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY(":status", "500"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("accept-language", ""),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("access-control-allow-credentials", "FALSE"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("access-control-allow-credentials", "TRUE"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("access-control-allow-headers", "*"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("access-control-allow-methods", "get"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("access-control-allow-methods", "get, post, options"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("access-control-allow-methods", "options"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("access-control-expose-headers", "content-length"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("access-control-request-headers", "content-type"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("access-control-request-method", "get"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("access-control-request-method", "post"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("alt-svc", "clear"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("authorization", ""),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("content-security-policy",
                                       "script-src 'none'; object-src 'none'; base-uri 'none'"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("early-data", "1"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("expect-ct", ""),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("forwarded", ""),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("if-range", ""),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("origin", ""),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("purpose", "prefetch"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("server", ""),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("timing-allow-origin", "*"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("upgrade-insecure-requests", "1"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("user-agent", ""),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("x-forwarded-for", ""),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("x-frame-options", "deny"),
        FIBER_HTTP3_QPACK_STATIC_ENTRY("x-frame-options", "sameorigin"),
};

bool Http3QpackStaticTable::get_by_index(std::uint32_t index, TableEntryView &view) noexcept {
    view = TableEntryView{};
    if (index >= kEntryCount) {
        return false;
    }

    const StaticEntry &entry = kEntries_[index];
    view.name = std::string_view(entry.name, entry.name_len);
    view.value = std::string_view(entry.value, entry.value_len);
    view.name_hash = entry.name_hash;
    return true;
}

Http3QpackStaticTable::FindResult Http3QpackStaticTable::find(std::string_view name, std::uint64_t name_hash,
                                                              std::string_view value) noexcept {
    FindResult name_match;
    for (std::uint32_t i = 0; i < kEntryCount; ++i) {
        const StaticEntry &entry = kEntries_[i];
        if (entry.name_hash != name_hash || entry.name_len != name.size()) {
            continue;
        }
        const std::string_view entry_name(entry.name, entry.name_len);
        if (!http_header_name_equals_ci(name, entry_name)) {
            continue;
        }

        const std::string_view entry_value(entry.value, entry.value_len);
        if (entry.value_len == value.size() && same_bytes(value, entry_value)) {
            return {
                    .kind = FindKind::EntryMatch,
                    .index = i,
                    .entry = {.name = entry_name, .value = entry_value, .name_hash = entry.name_hash},
            };
        }

        if (name_match.kind == FindKind::NoMatch) {
            name_match.kind = FindKind::NameMatch;
            name_match.index = i;
            name_match.entry.name = entry_name;
            name_match.entry.value = entry_value;
            name_match.entry.name_hash = entry.name_hash;
        }
    }
    return name_match;
}

#undef FIBER_HTTP3_QPACK_STATIC_ENTRY

} // namespace fiber::http
