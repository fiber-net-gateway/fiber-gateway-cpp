#ifndef FIBER_HTTP_HEADER_HASH_H
#define FIBER_HTTP_HEADER_HASH_H

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fiber::http {

inline constexpr uint64_t http_header_name_to_lowercase_and_hash(std::string_view name, char *dst) noexcept {
    std::uint32_t hash = 0;
    for (std::size_t i = 0; i < name.size(); ++i) {
        unsigned char lower = static_cast<unsigned char>(name[i]);
        if (lower >= 'A' && lower <= 'Z') {
            lower = static_cast<unsigned char>(lower - 'A' + 'a');
        }
        if (dst) {
            dst[i] = static_cast<char>(lower);
        }
        hash = hash * 31 + lower;
    }
    return static_cast<std::uint64_t>(hash);
}

inline constexpr uint64_t http_header_name_hash(std::string_view name) noexcept {
    return http_header_name_to_lowercase_and_hash(name, nullptr);
}

inline constexpr bool http_header_name_equals_ci(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        unsigned char left = static_cast<unsigned char>(a[i]);
        unsigned char right = static_cast<unsigned char>(b[i]);
        if (left >= 'A' && left <= 'Z') {
            left = static_cast<unsigned char>(left - 'A' + 'a');
        }
        if (right >= 'A' && right <= 'Z') {
            right = static_cast<unsigned char>(right - 'A' + 'a');
        }
        if (left != right) {
            return false;
        }
    }
    return true;
}

} // namespace fiber::http

#endif // FIBER_HTTP_HEADER_HASH_H
