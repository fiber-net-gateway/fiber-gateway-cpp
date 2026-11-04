#ifndef FIBER_REGEX_RUNE_H
#define FIBER_REGEX_RUNE_H

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fiber::regex::detail {

using Rune = std::int32_t;

inline constexpr Rune kRuneMax = 0x10FFFF;
inline constexpr Rune kRuneSelf = 0x80; // runes below this are one byte in UTF-8
inline constexpr std::size_t kUtfMax = 4;

// Decodes the code point at the front of `s`. Returns its length in bytes, or 0
// for a malformed sequence (overlong forms and surrogates included).
[[nodiscard]] inline std::size_t decode_utf8(std::string_view s, Rune &out) noexcept {
    if (s.empty()) {
        return 0;
    }
    const auto byte = [s](std::size_t i) noexcept {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(s[i]));
    };
    const auto continuation = [&byte](std::size_t i) noexcept { return (byte(i) & 0xC0U) == 0x80U; };

    const std::uint32_t b0 = byte(0);
    if (b0 < 0x80U) {
        out = static_cast<Rune>(b0);
        return 1;
    }
    if (b0 < 0xC2U) {
        return 0;
    }
    if (b0 < 0xE0U) {
        if (s.size() < 2 || !continuation(1)) {
            return 0;
        }
        out = static_cast<Rune>(((b0 & 0x1FU) << 6U) | (byte(1) & 0x3FU));
        return 2;
    }
    if (b0 < 0xF0U) {
        if (s.size() < 3 || !continuation(1) || !continuation(2)) {
            return 0;
        }
        const std::uint32_t r = ((b0 & 0x0FU) << 12U) | ((byte(1) & 0x3FU) << 6U) | (byte(2) & 0x3FU);
        if (r < 0x800U || (r >= 0xD800U && r <= 0xDFFFU)) {
            return 0;
        }
        out = static_cast<Rune>(r);
        return 3;
    }
    if (b0 < 0xF5U) {
        if (s.size() < 4 || !continuation(1) || !continuation(2) || !continuation(3)) {
            return 0;
        }
        const std::uint32_t r =
                ((b0 & 0x07U) << 18U) | ((byte(1) & 0x3FU) << 12U) | ((byte(2) & 0x3FU) << 6U) | (byte(3) & 0x3FU);
        if (r < 0x10000U || r > static_cast<std::uint32_t>(kRuneMax)) {
            return 0;
        }
        out = static_cast<Rune>(r);
        return 4;
    }
    return 0;
}

// Encodes `r` into `out` (at least kUtfMax bytes) and returns the length.
inline std::size_t encode_utf8(Rune r, std::uint8_t *out) noexcept {
    const auto u = static_cast<std::uint32_t>(r);
    if (u < 0x80U) {
        out[0] = static_cast<std::uint8_t>(u);
        return 1;
    }
    if (u < 0x800U) {
        out[0] = static_cast<std::uint8_t>(0xC0U | (u >> 6U));
        out[1] = static_cast<std::uint8_t>(0x80U | (u & 0x3FU));
        return 2;
    }
    if (u < 0x10000U) {
        out[0] = static_cast<std::uint8_t>(0xE0U | (u >> 12U));
        out[1] = static_cast<std::uint8_t>(0x80U | ((u >> 6U) & 0x3FU));
        out[2] = static_cast<std::uint8_t>(0x80U | (u & 0x3FU));
        return 3;
    }
    out[0] = static_cast<std::uint8_t>(0xF0U | (u >> 18U));
    out[1] = static_cast<std::uint8_t>(0x80U | ((u >> 12U) & 0x3FU));
    out[2] = static_cast<std::uint8_t>(0x80U | ((u >> 6U) & 0x3FU));
    out[3] = static_cast<std::uint8_t>(0x80U | (u & 0x3FU));
    return 4;
}

} // namespace fiber::regex::detail

#endif // FIBER_REGEX_RUNE_H
