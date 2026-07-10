#include "Base64.h"

namespace fiber::common::base64 {

namespace {

constexpr char kEnc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Value of a base64 alphabet char (0-63), or -1 for '=' / anything else.
constexpr int decode_digit(unsigned char c) noexcept {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

} // namespace

std::string base64_encode(const std::uint8_t *data, std::size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 2 < len) {
        std::uint32_t triple = (static_cast<std::uint32_t>(data[i]) << 16) |
                               (static_cast<std::uint32_t>(data[i + 1]) << 8) | static_cast<std::uint32_t>(data[i + 2]);
        out.push_back(kEnc[(triple >> 18) & 0x3F]);
        out.push_back(kEnc[(triple >> 12) & 0x3F]);
        out.push_back(kEnc[(triple >> 6) & 0x3F]);
        out.push_back(kEnc[triple & 0x3F]);
        i += 3;
    }
    if (i < len) {
        std::uint32_t triple = static_cast<std::uint32_t>(data[i]) << 16;
        out.push_back(kEnc[(triple >> 18) & 0x3F]);
        if (i + 1 < len) {
            triple |= static_cast<std::uint32_t>(data[i + 1]) << 8;
            out.push_back(kEnc[(triple >> 12) & 0x3F]);
            out.push_back(kEnc[(triple >> 6) & 0x3F]);
            out.push_back('=');
        } else {
            out.push_back(kEnc[(triple >> 12) & 0x3F]);
            out.push_back('=');
            out.push_back('=');
        }
    }
    return out;
}

bool base64_decode(std::string_view in, std::string &out) noexcept {
    out.clear();
    if (in.empty()) {
        return true;
    }
    if (in.size() % 4 != 0) {
        return false;
    }
    // Padding: trailing '=' only, count 0/1/2; none may appear before the run.
    std::size_t pad = 0;
    while (pad < in.size() && in[in.size() - 1 - pad] == '=') {
        ++pad;
    }
    if (pad > 2) {
        return false;
    }
    for (std::size_t i = 0; i < in.size() - pad; ++i) {
        if (in[i] == '=') {
            return false; // '=' outside the trailing pad run
        }
    }

    out.reserve((in.size() / 4) * 3);
    const std::size_t n = in.size();
    for (std::size_t i = 0; i < n; i += 4) {
        const bool last = (i + 4 == n);
        const int s0 = decode_digit(static_cast<unsigned char>(in[i]));
        const int s1 = decode_digit(static_cast<unsigned char>(in[i + 1]));
        if (s0 < 0 || s1 < 0) {
            return false;
        }
        out.push_back(static_cast<char>(((s0 << 2) | (s1 >> 4)) & 0xFF));
        if (last && pad >= 2) {
            continue; // "XX==" -> 1 byte
        }
        const int s2 = decode_digit(static_cast<unsigned char>(in[i + 2]));
        if (s2 < 0) {
            return false;
        }
        out.push_back(static_cast<char>(((s1 << 4) | (s2 >> 2)) & 0xFF));
        if (last && pad >= 1) {
            continue; // "XXX=" -> 2 bytes
        }
        const int s3 = decode_digit(static_cast<unsigned char>(in[i + 3]));
        if (s3 < 0) {
            return false;
        }
        out.push_back(static_cast<char>(((s2 << 6) | s3) & 0xFF));
    }
    return true;
}

} // namespace fiber::common::base64
