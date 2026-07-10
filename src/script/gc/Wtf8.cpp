#include "Wtf8.h"

#include <cstring>
#include <limits>

namespace fiber::script::gc_detail {

namespace {

bool is_high_surrogate(std::uint32_t unit) noexcept { return unit >= 0xD800 && unit <= 0xDBFF; }

bool is_low_surrogate(std::uint32_t unit) noexcept { return unit >= 0xDC00 && unit <= 0xDFFF; }

bool checked_add(std::size_t lhs, std::size_t rhs, std::size_t &out) noexcept {
    if (lhs > std::numeric_limits<std::size_t>::max() - rhs) {
        return false;
    }
    out = lhs + rhs;
    return true;
}

bool next_codepoint(const char *data, std::size_t len, std::size_t &pos, std::uint32_t &codepoint) noexcept {
    if (!data || pos >= len) {
        return false;
    }
    const unsigned char ch = static_cast<unsigned char>(data[pos]);
    if (ch < 0x80) {
        codepoint = ch;
        ++pos;
        return true;
    }

    std::uint8_t needed = 0;
    std::uint32_t value = 0;
    std::uint32_t min_value = 0;
    if ((ch & 0xE0) == 0xC0) {
        needed = 1;
        value = ch & 0x1F;
        min_value = 0x80;
    } else if ((ch & 0xF0) == 0xE0) {
        needed = 2;
        value = ch & 0x0F;
        min_value = 0x800;
    } else if ((ch & 0xF8) == 0xF0) {
        needed = 3;
        value = ch & 0x07;
        min_value = 0x10000;
    } else {
        return false;
    }
    if (needed >= len - pos) {
        return false;
    }
    for (std::uint8_t i = 1; i <= needed; ++i) {
        const unsigned char next = static_cast<unsigned char>(data[pos + i]);
        if ((next & 0xC0) != 0x80) {
            return false;
        }
        value = (value << 6) | (next & 0x3F);
    }
    if (value < min_value || value > 0x10FFFF) {
        return false;
    }
    pos += static_cast<std::size_t>(needed) + 1;
    codepoint = value;
    return true;
}

std::size_t utf16_encoded_len(std::uint32_t unit) noexcept {
    if (unit < 0x80) {
        return 1;
    }
    if (unit < 0x800) {
        return 2;
    }
    return 3;
}

} // namespace

std::uint8_t wtf8_write_codepoint(std::uint32_t codepoint, char *out) noexcept {
    if (codepoint < 0x80) {
        out[0] = static_cast<char>(codepoint);
        return 1;
    }
    if (codepoint < 0x800) {
        out[0] = static_cast<char>(0xC0 | (codepoint >> 6));
        out[1] = static_cast<char>(0x80 | (codepoint & 0x3F));
        return 2;
    }
    if (codepoint < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (codepoint >> 12));
        out[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | (codepoint >> 18));
    out[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (codepoint & 0x3F));
    return 4;
}

bool wtf8_measure_utf16(const char16_t *data, std::size_t len, Wtf8MeasureResult &out) noexcept {
    out = {};
    if (len > 0 && !data) {
        return false;
    }
    for (std::size_t i = 0; i < len; ++i) {
        const std::uint32_t unit = data[i];
        std::size_t encoded_len = utf16_encoded_len(unit);
        if (is_high_surrogate(unit)) {
            if (i + 1 < len && is_low_surrogate(data[i + 1])) {
                encoded_len = 4;
                ++i;
            } else {
                out.well_formed = false;
            }
        } else if (is_low_surrogate(unit)) {
            out.well_formed = false;
        }
        if (!checked_add(out.byte_len, encoded_len, out.byte_len)) {
            return false;
        }
    }
    return true;
}

bool wtf8_write_utf16(const char16_t *data, std::size_t len, char *dst, std::size_t dst_len) noexcept {
    if ((len > 0 && !data) || (dst_len > 0 && !dst)) {
        return false;
    }
    std::size_t offset = 0;
    for (std::size_t i = 0; i < len; ++i) {
        std::uint32_t codepoint = data[i];
        if (is_high_surrogate(codepoint) && i + 1 < len && is_low_surrogate(data[i + 1])) {
            codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (static_cast<std::uint32_t>(data[++i]) - 0xDC00);
        }
        char encoded[4];
        const std::uint8_t count = wtf8_write_codepoint(codepoint, encoded);
        if (offset > dst_len || count > dst_len - offset) {
            return false;
        }
        std::memcpy(dst + offset, encoded, count);
        offset += count;
    }
    return offset == dst_len;
}

bool wtf8_measure_latin1(const std::uint8_t *data, std::size_t len, std::size_t &byte_len) noexcept {
    byte_len = len;
    if (len > 0 && !data) {
        return false;
    }
    for (std::size_t i = 0; i < len; ++i) {
        if (data[i] >= 0x80 && !checked_add(byte_len, 1, byte_len)) {
            return false;
        }
    }
    return true;
}

bool wtf8_write_latin1(const std::uint8_t *data, std::size_t len, char *dst, std::size_t dst_len) noexcept {
    if ((len > 0 && !data) || (dst_len > 0 && !dst)) {
        return false;
    }
    std::size_t offset = 0;
    for (std::size_t i = 0; i < len; ++i) {
        char encoded[2];
        const std::uint8_t count = wtf8_write_codepoint(data[i], encoded);
        if (offset > dst_len || count > dst_len - offset) {
            return false;
        }
        std::memcpy(dst + offset, encoded, count);
        offset += count;
    }
    return offset == dst_len;
}

bool wtf8_next_utf16_unit(Wtf8Cursor &cursor, char16_t &unit) noexcept {
    if (cursor.has_pending) {
        unit = cursor.pending;
        cursor.has_pending = false;
        return true;
    }
    if (cursor.pos >= cursor.len) {
        return false;
    }
    std::uint32_t codepoint = 0;
    if (!next_codepoint(cursor.data, cursor.len, cursor.pos, codepoint)) {
        cursor.malformed = true;
        return false;
    }
    if (codepoint <= 0xFFFF) {
        unit = static_cast<char16_t>(codepoint);
        return true;
    }
    const std::uint32_t value = codepoint - 0x10000;
    unit = static_cast<char16_t>(0xD800 + (value >> 10));
    cursor.pending = static_cast<char16_t>(0xDC00 + (value & 0x3FF));
    cursor.has_pending = true;
    return true;
}

bool wtf8_is_well_formed(const char *data, std::size_t len) noexcept {
    if (len > 0 && !data) {
        return false;
    }
    std::size_t pos = 0;
    while (pos < len) {
        std::uint32_t codepoint = 0;
        if (!next_codepoint(data, len, pos, codepoint) || is_high_surrogate(codepoint) || is_low_surrogate(codepoint)) {
            return false;
        }
    }
    return true;
}

bool wtf8_starts_with_low_surrogate(const char *data, std::size_t len, char16_t &unit) noexcept {
    if (len == 0 || !data) {
        return false;
    }
    std::size_t pos = 0;
    std::uint32_t codepoint = 0;
    if (!next_codepoint(data, len, pos, codepoint) || !is_low_surrogate(codepoint)) {
        return false;
    }
    unit = static_cast<char16_t>(codepoint);
    return true;
}

bool wtf8_ends_with_high_surrogate(const char *data, std::size_t len, char16_t &unit) noexcept {
    if (len == 0 || !data) {
        return false;
    }
    std::size_t start = len - 1;
    while (start > 0 && (static_cast<unsigned char>(data[start]) & 0xC0) == 0x80) {
        --start;
    }
    std::size_t pos = start;
    std::uint32_t codepoint = 0;
    if (!next_codepoint(data, len, pos, codepoint) || pos != len || !is_high_surrogate(codepoint)) {
        return false;
    }
    unit = static_cast<char16_t>(codepoint);
    return true;
}

bool wtf8_plan_utf16_slice(const char *data, std::size_t byte_len, std::size_t utf16_len, std::size_t begin,
                           std::size_t end, Wtf8SlicePlan &out) noexcept {
    out = {};
    if (begin > end || end > utf16_len || (byte_len > 0 && !data)) {
        return false;
    }
    if (begin == end) {
        return true;
    }

    std::size_t pos = 0;
    std::size_t unit_pos = 0;
    bool begin_set = false;
    while (pos < byte_len) {
        const std::size_t raw_begin = pos;
        std::uint32_t codepoint = 0;
        if (!next_codepoint(data, byte_len, pos, codepoint)) {
            return false;
        }
        const std::size_t units = codepoint > 0xFFFF ? 2 : 1;

        if (!begin_set) {
            if (begin == unit_pos) {
                out.copy_begin = raw_begin;
                begin_set = true;
            } else if (units == 2 && begin == unit_pos + 1) {
                const std::uint32_t value = codepoint - 0x10000;
                out.leading_low_surrogate = true;
                out.leading_unit = static_cast<char16_t>(0xDC00 + (value & 0x3FF));
                out.copy_begin = pos;
                begin_set = true;
            }
        }

        if (end == unit_pos) {
            out.copy_end = raw_begin;
            return begin_set;
        }
        if (units == 2 && end == unit_pos + 1) {
            const std::uint32_t value = codepoint - 0x10000;
            out.trailing_high_surrogate = true;
            out.trailing_unit = static_cast<char16_t>(0xD800 + (value >> 10));
            out.copy_end = raw_begin;
            return begin_set;
        }
        unit_pos += units;
        if (end == unit_pos) {
            out.copy_end = pos;
            return begin_set;
        }
    }
    return false;
}

} // namespace fiber::script::gc_detail
