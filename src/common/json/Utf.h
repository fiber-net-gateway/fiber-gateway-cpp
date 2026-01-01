//
// Created by dear on 2025/12/30.
//

#ifndef FIBER_UTF_H
#define FIBER_UTF_H

#include <cstddef>
#include <cstdint>

namespace fiber::json {

struct Utf8ScanResult {
    std::size_t utf16_len = 0;
    bool all_byte = true;
};

[[nodiscard]] bool utf8_next_codepoint(const char *data, std::size_t len, std::size_t &pos, std::uint32_t &codepoint);
[[nodiscard]] bool utf8_scan(const char *data, std::size_t len, Utf8ScanResult &out);
[[nodiscard]] bool utf8_validate(const char *data, std::size_t len);
[[nodiscard]] bool utf8_write_bytes(const char *data, std::size_t len, std::uint8_t *dst, std::size_t dst_len);
[[nodiscard]] bool utf8_write_utf16(const char *data, std::size_t len, char16_t *dst, std::size_t dst_len);

} // namespace fiber::json

#endif // FIBER_UTF_H
