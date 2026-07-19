#include "Http2HpackStaticTable.h"

#include <array>
#include <cstring>

#include "HttpHeaderHash.h"

namespace fiber::http {

namespace {

struct StaticEntry {
    const char *name = nullptr;
    const char *value = nullptr;
    std::uint32_t name_hash = 0;
    std::uint16_t name_len = 0;
    std::uint16_t value_len = 0;
};

constexpr std::size_t kNameIndexCapacity = 128;
constexpr std::uint8_t kEmptyIndex = 0;

using NameIndex = std::array<std::uint8_t, kNameIndexCapacity>;

static_assert((kNameIndexCapacity & (kNameIndexCapacity - 1)) == 0);
static_assert(kNameIndexCapacity > Http2HpackStaticTable::kEntryCount);
static_assert(Http2HpackStaticTable::kEntryCount < 0xffU);

#define FIBER_HTTP2_STATIC_ENTRY(name_literal, value_literal)                                                          \
    {name_literal, value_literal, static_cast<std::uint32_t>(fiber::http::http_header_name_hash(name_literal)),        \
     static_cast<std::uint16_t>(sizeof(name_literal) - 1), static_cast<std::uint16_t>(sizeof(value_literal) - 1)}

constexpr std::array<StaticEntry, Http2HpackStaticTable::kEntryCount> kEntries{{
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
}};

[[nodiscard]] constexpr bool same_static_name(const StaticEntry &left, const StaticEntry &right) noexcept {
    if (left.name_hash != right.name_hash || left.name_len != right.name_len) {
        return false;
    }
    for (std::size_t i = 0; i < left.name_len; ++i) {
        if (left.name[i] != right.name[i]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] consteval bool names_are_contiguous() {
    for (std::size_t id = 1; id < kEntries.size(); ++id) {
        for (std::size_t previous = 0; previous + 1 < id; ++previous) {
            if (same_static_name(kEntries[id], kEntries[previous]) &&
                !same_static_name(kEntries[id], kEntries[id - 1])) {
                return false;
            }
        }
    }
    return true;
}

static_assert(names_are_contiguous());

[[nodiscard]] consteval NameIndex make_name_index() {
    NameIndex slots{};
    for (std::size_t id = 0; id < kEntries.size(); ++id) {
        if (id != 0 && same_static_name(kEntries[id], kEntries[id - 1])) {
            continue;
        }

        std::size_t slot = kEntries[id].name_hash & (kNameIndexCapacity - 1);
        while (slots[slot] != kEmptyIndex) {
            slot = (slot + 1) & (kNameIndexCapacity - 1);
        }
        slots[slot] = static_cast<std::uint8_t>(id + 1);
    }
    return slots;
}

constexpr NameIndex kNameIndex = make_name_index();

[[nodiscard]] inline bool same_bytes(std::string_view left, std::string_view right) noexcept {
    return left.size() == right.size() && (right.empty() || std::memcmp(left.data(), right.data(), right.size()) == 0);
}

[[nodiscard]] std::uint8_t find_first_name_index(std::string_view name, std::uint64_t name_hash) noexcept {
    std::size_t slot = static_cast<std::uint32_t>(name_hash) & (kNameIndexCapacity - 1);
    for (std::size_t probe = 0; probe < kNameIndexCapacity; ++probe) {
        const std::uint8_t index = kNameIndex[slot];
        if (index == kEmptyIndex) {
            return 0;
        }

        const StaticEntry &entry = kEntries[index - 1];
        if (entry.name_hash == name_hash && entry.name_len == name.size() &&
            http_header_name_equals_ci(name, std::string_view(entry.name, entry.name_len))) {
            return index;
        }
        slot = (slot + 1) & (kNameIndexCapacity - 1);
    }
    return 0;
}

} // namespace

bool Http2HpackStaticTable::get_by_index(std::uint32_t index, TableEntryView &view) noexcept {
    view = TableEntryView{};
    if (index == 0 || index > kEntryCount) {
        return false;
    }

    const StaticEntry &entry = kEntries[index - 1];
    view.name = std::string_view(entry.name, entry.name_len);
    view.value = std::string_view(entry.value, entry.value_len);
    view.name_hash = entry.name_hash;
    view.storage = Http2HpackEntryStorage::Static;
    return true;
}

Http2HpackStaticTable::FindResult Http2HpackStaticTable::find(std::string_view name, std::uint64_t name_hash,
                                                              std::string_view value) noexcept {
    FindResult result;
    result.name_index = find_first_name_index(name, name_hash);
    if (result.name_index == 0) {
        return result;
    }

    const StaticEntry &first = kEntries[result.name_index - 1];
    for (std::uint32_t index = result.name_index; index <= kEntryCount; ++index) {
        const StaticEntry &entry = kEntries[index - 1];
        if (!same_static_name(first, entry)) {
            break;
        }
        if (same_bytes(value, std::string_view(entry.value, entry.value_len))) {
            result.exact_index = static_cast<std::uint8_t>(index);
            break;
        }
    }
    return result;
}

bool Http2HpackStaticTable::find_name(std::string_view name, std::uint32_t &index) noexcept {
    return find_name(name, http_header_name_hash(name), index);
}

bool Http2HpackStaticTable::find_name(std::string_view name, std::uint64_t name_hash, std::uint32_t &index) noexcept {
    const std::uint8_t found = find_first_name_index(name, name_hash);
    if (found == 0) {
        return false;
    }
    index = found;
    return true;
}

#undef FIBER_HTTP2_STATIC_ENTRY

} // namespace fiber::http
