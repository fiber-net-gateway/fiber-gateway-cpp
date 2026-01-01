//
// Created by dear on 2025/12/30.
//

#include "Utf.h"

namespace fiber::json {

bool utf8_next_codepoint(const char *data, std::size_t len, std::size_t &pos, std::uint32_t &codepoint) {
    unsigned char ch = static_cast<unsigned char>(data[pos]);
    if (ch < 0x80) {
        codepoint = ch;
        pos += 1;
        return true;
    }
    int needed = 0;
    std::uint32_t code = 0;
    std::uint32_t min_value = 0;
    if ((ch & 0xE0) == 0xC0) {
        needed = 1;
        code = ch & 0x1F;
        min_value = 0x80;
    } else if ((ch & 0xF0) == 0xE0) {
        needed = 2;
        code = ch & 0x0F;
        min_value = 0x800;
    } else if ((ch & 0xF8) == 0xF0) {
        needed = 3;
        code = ch & 0x07;
        min_value = 0x10000;
    } else {
        return false;
    }
    if (pos + static_cast<std::size_t>(needed) >= len) {
        return false;
    }
    for (int idx = 1; idx <= needed; ++idx) {
        unsigned char next = static_cast<unsigned char>(data[pos + idx]);
        if ((next & 0xC0) != 0x80) {
            return false;
        }
        code = (code << 6) | (next & 0x3F);
    }
    if (code < min_value || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
        return false;
    }
    pos += static_cast<std::size_t>(needed) + 1;
    codepoint = code;
    return true;
}

bool utf8_scan(const char *data, std::size_t len, Utf8ScanResult &out) {
    out = {};
    if (len == 0) {
        return true;
    }
    if (!data) {
        return false;
    }
    std::size_t pos = 0;
    while (pos < len) {
        std::uint32_t codepoint = 0;
        if (!utf8_next_codepoint(data, len, pos, codepoint)) {
            return false;
        }
        if (codepoint > 0xFF) {
            out.all_byte = false;
        }
        out.utf16_len += (codepoint <= 0xFFFF) ? 1 : 2;
    }
    return true;
}

bool utf8_validate(const char *data, std::size_t len) {
    Utf8ScanResult tmp;
    return utf8_scan(data, len, tmp);
}

bool utf8_write_bytes(const char *data, std::size_t len, std::uint8_t *dst, std::size_t dst_len) {
    if (len == 0) {
        return true;
    }
    if (!data || (!dst && dst_len > 0)) {
        return false;
    }
    std::size_t pos = 0;
    std::size_t out_pos = 0;
    while (pos < len) {
        std::uint32_t codepoint = 0;
        if (!utf8_next_codepoint(data, len, pos, codepoint)) {
            return false;
        }
        if (codepoint > 0xFF) {
            return false;
        }
        if (out_pos >= dst_len) {
            return false;
        }
        dst[out_pos++] = static_cast<std::uint8_t>(codepoint);
    }
    return out_pos == dst_len;
}

bool utf8_write_utf16(const char *data, std::size_t len, char16_t *dst, std::size_t dst_len) {
    if (len == 0) {
        return true;
    }
    if (!data || (!dst && dst_len > 0)) {
        return false;
    }
    std::size_t pos = 0;
    std::size_t out_pos = 0;
    while (pos < len) {
        std::uint32_t codepoint = 0;
        if (!utf8_next_codepoint(data, len, pos, codepoint)) {
            return false;
        }
        if (codepoint <= 0xFFFF) {
            if (out_pos >= dst_len) {
                return false;
            }
            dst[out_pos++] = static_cast<char16_t>(codepoint);
            continue;
        }
        std::uint32_t value = codepoint - 0x10000;
        char16_t high = static_cast<char16_t>(0xD800 + (value >> 10));
        char16_t low = static_cast<char16_t>(0xDC00 + (value & 0x3FF));
        if (out_pos + 1 >= dst_len) {
            return false;
        }
        dst[out_pos++] = high;
        dst[out_pos++] = low;
    }
    return out_pos == dst_len;
}

} // namespace fiber::json
