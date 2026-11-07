//
// Created by dear on 2025/12/30.
//

#ifndef FIBER_UTF_H
#define FIBER_UTF_H

#include <cstddef>
#include <cstdint>
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

// Two-phase repair of percent-decoded bytes before they are stored in a GcString (which
// rejects invalid UTF-8): valid UTF-8 sequences are copied verbatim and each ill-formed
// byte becomes one U+FFFD (EF BF BD), the skip-one policy of the common REPLACE decoder.
// utf8_repaired_size gives the exact output size; utf8_repair_into writes that many bytes
// to dst, which must not overlap src. Neither allocates.
[[nodiscard]] std::size_t utf8_repaired_size(std::string_view src) noexcept;
void utf8_repair_into(std::string_view src, char *dst) noexcept;

} // namespace fiber::json

#endif // FIBER_UTF_H
