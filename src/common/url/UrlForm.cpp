#include "UrlForm.h"

#include "../json/Utf.h"

namespace fiber::common::url {

namespace {

// 0..15 for a hex digit, or 0xFF if not hex (both cases accepted, matching Java
// UrlEncoded.convertHexDigit).
constexpr unsigned char hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return static_cast<unsigned char>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<unsigned char>(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<unsigned char>(c - 'A' + 10);
    }
    return 0xFF;
}

bool is_unreserved(unsigned char b) noexcept {
    return (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') || b == '-' || b == '_' ||
           b == '.' || b == '*';
}

} // namespace

void form_encode(std::string_view in, std::string &out) {
    static constexpr char kHex[] = "0123456789ABCDEF"; // uppercase, matching Java caseDiff
    for (unsigned char b: in) {
        if (b == ' ') {
            out.push_back('+');
        } else if (is_unreserved(b)) {
            out.push_back(static_cast<char>(b));
        } else {
            out.push_back('%');
            out.push_back(kHex[(b >> 4) & 0x0F]);
            out.push_back(kHex[b & 0x0F]);
        }
    }
}

bool form_decode_into(std::string_view in, std::string &out) {
    out.clear();
    const std::size_t len = in.size();
    for (std::size_t i = 0; i < len; ++i) {
        char c = in[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%') {
            // Need two hex digits after '%'. Java's decodeUtf8To requires i+2 < end.
            if (i + 2 >= len) {
                return false;
            }
            unsigned char hi = hex_digit(in[i + 1]);
            unsigned char lo = hex_digit(in[i + 2]);
            if (hi == 0xFF || lo == 0xFF) {
                return false;
            }
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
        } else {
            out.push_back(c);
        }
    }
    // Replace malformed UTF-8 with U+FFFD so the result is storable in a GcString.
    if (!fiber::json::utf8_validate(out.data(), out.size())) {
        std::string repaired;
        fiber::json::utf8_repair(out, repaired);
        out = std::move(repaired);
    }
    return true;
}

fiber::common::IoResult<std::string> form_decode(std::string_view in) {
    std::string out;
    if (!form_decode_into(in, out)) {
        return std::unexpected(fiber::common::IoErr::Invalid);
    }
    return out;
}

} // namespace fiber::common::url
