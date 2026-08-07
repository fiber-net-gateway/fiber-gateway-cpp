//
// Created by dear on 2025/12/30.
//

#ifndef FIBER_UTF_H
#define FIBER_UTF_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace fiber::json {

struct Utf8ScanResult {
    std::size_t utf16_len = 0;
    bool all_byte = true;
};

[[nodiscard]] bool utf8_next_codepoint(const char *data, std::size_t len, std::size_t &pos,
                                       std::uint32_t &codepoint) noexcept;
[[nodiscard]] bool utf8_scan(const char *data, std::size_t len, Utf8ScanResult &out) noexcept;
[[nodiscard]] bool utf8_validate(const char *data, std::size_t len) noexcept;
[[nodiscard]] bool utf8_write_bytes(const char *data, std::size_t len, std::uint8_t *dst, std::size_t dst_len) noexcept;
[[nodiscard]] bool utf8_write_utf16(const char *data, std::size_t len, char16_t *dst, std::size_t dst_len) noexcept;

// Appends src to out, copying valid UTF-8 sequences verbatim and replacing each
// malformed sequence with U+FFFD (EF BF BD). Used to repair percent-decoded bytes
// before they are stored in a GcString (which rejects invalid UTF-8). out is not
// cleared; callers may pre-clear. A single U+FFFD is emitted per ill-formed byte
// (skip-one policy), matching the common REPLACE decoder behaviour.
void utf8_repair(std::string_view src, std::string &out);

} // namespace fiber::json

#endif // FIBER_UTF_H
