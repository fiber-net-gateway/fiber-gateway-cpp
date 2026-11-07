#include <fiber/common/util/UrlForm.h>

#include <fiber/common/json/Utf.h>

namespace fiber::util {

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

bool form_percent_decode(std::string_view in, char *dst, std::size_t &len) noexcept {
    const std::size_t in_len = in.size();
    std::size_t written = 0;
    for (std::size_t i = 0; i < in_len; ++i) {
        char c = in[i];
        if (c == '+') {
            dst[written++] = ' ';
        } else if (c == '%') {
            // Need two hex digits after '%'. Java's decodeUtf8To requires i+2 < end.
            if (i + 2 >= in_len) {
                return false;
            }
            unsigned char hi = hex_digit(in[i + 1]);
            unsigned char lo = hex_digit(in[i + 2]);
            if (hi == 0xFF || lo == 0xFF) {
                return false;
            }
            dst[written++] = static_cast<char>((hi << 4) | lo);
            i += 2;
        } else {
            dst[written++] = c;
        }
    }
    len = written;
    return true;
}

bool form_decode_into(std::string_view in, std::string &out) {
    out.resize(in.size());
    std::size_t len = 0;
    if (!form_percent_decode(in, out.data(), len)) {
        out.clear();
        return false;
    }
    out.resize(len);
    // Replace malformed UTF-8 with U+FFFD so the result is storable in a GcString.
    if (!fiber::json::utf8_validate(out.data(), out.size())) {
        std::string repaired(fiber::json::utf8_repaired_size(out), '\0');
        fiber::json::utf8_repair_into(out, repaired.data());
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

} // namespace fiber::util
