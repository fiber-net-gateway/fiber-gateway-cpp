#include <fiber/common/util/Base64.h>

namespace fiber::util {

namespace {

constexpr char kEnc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// 256-entry reverse lookup: 6-bit value for alphabet chars, 0xFF for everything
// else (including '=' and whitespace). One table load replaces the comparison
// chain per character, and four values OR-ed together test validity in a single
// branch (valid values are <= 0x3F, the invalid sentinel has bit 7 set).
struct DecTable {
    std::uint8_t v[256];

    constexpr DecTable() : v{} {
        for (auto &entry: v) {
            entry = 0xFF;
        }
        std::uint8_t d = 0;
        for (char c = 'A'; c <= 'Z'; ++c) {
            v[static_cast<unsigned char>(c)] = d++;
        }
        for (char c = 'a'; c <= 'z'; ++c) {
            v[static_cast<unsigned char>(c)] = d++;
        }
        for (char c = '0'; c <= '9'; ++c) {
            v[static_cast<unsigned char>(c)] = d++;
        }
        v[static_cast<unsigned char>('+')] = 62;
        v[static_cast<unsigned char>('/')] = 63;
    }
};
constexpr DecTable kDec{};

// Big-endian 24-bit triple from three consecutive bytes.
constexpr std::uint32_t load_triple(const std::uint8_t *p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 16) | (static_cast<std::uint32_t>(p[1]) << 8) |
           static_cast<std::uint32_t>(p[2]);
}

// Emits the four alphabet chars for one 24-bit triple.
constexpr void enc_triple(std::uint32_t t, char *out) noexcept {
    out[0] = kEnc[(t >> 18) & 0x3F];
    out[1] = kEnc[(t >> 12) & 0x3F];
    out[2] = kEnc[(t >> 6) & 0x3F];
    out[3] = kEnc[t & 0x3F];
}

// Structural validation shared by base64_decoded_size and base64_decode_into:
// length % 4, '=' confined to a trailing run of at most two. Returns the exact
// decoded byte count and true, or 0 and false.
[[nodiscard]] std::size_t decoded_structural_size(std::string_view in, bool &ok) noexcept {
    ok = true;
    if (in.empty()) {
        return 0;
    }
    if (in.size() % 4 != 0) {
        ok = false;
        return 0;
    }
    // Padding: trailing '=' only, count 0/1/2; none may appear before the run.
    std::size_t pad = 0;
    while (pad < in.size() && in[in.size() - 1 - pad] == '=') {
        ++pad;
    }
    if (pad > 2) {
        ok = false;
        return 0;
    }
    for (std::size_t i = 0; i < in.size() - pad; ++i) {
        if (in[i] == '=') {
            ok = false; // '=' outside the trailing pad run
            return 0;
        }
    }
    return ((in.size() / 4) * 3) - pad;
}

} // namespace

std::size_t base64_decoded_size(std::string_view in, bool &ok) noexcept { return decoded_structural_size(in, ok); }

void base64_encode_into(const std::uint8_t *data, std::size_t len, char *dst) noexcept {
    std::size_t o = 0;
    std::size_t i = 0;
    // Four triples (12 bytes -> 16 chars) per iteration: fewer loop branches and
    // independent shift chains; then any remaining full triple.
    while (i + 12 <= len) {
        enc_triple(load_triple(data + i), dst + o);
        enc_triple(load_triple(data + i + 3), dst + o + 4);
        enc_triple(load_triple(data + i + 6), dst + o + 8);
        enc_triple(load_triple(data + i + 9), dst + o + 12);
        i += 12;
        o += 16;
    }
    while (i + 3 <= len) {
        enc_triple(load_triple(data + i), dst + o);
        i += 3;
        o += 4;
    }
    // Tail of 1 or 2 bytes, zero-filled to a triple so the same shifts apply.
    if (i < len) {
        const std::uint32_t triple = (static_cast<std::uint32_t>(data[i]) << 16) |
                                     (i + 1 < len ? static_cast<std::uint32_t>(data[i + 1]) << 8 : 0U);
        dst[o++] = kEnc[(triple >> 18) & 0x3F];
        dst[o++] = kEnc[(triple >> 12) & 0x3F];
        if (i + 1 < len) {
            dst[o++] = kEnc[(triple >> 6) & 0x3F];
            dst[o++] = '=';
        } else {
            dst[o++] = '=';
            dst[o++] = '=';
        }
    }
}

bool base64_decode_into(std::string_view in, std::uint8_t *dst) noexcept {
    bool ok = false;
    const std::size_t out_len = decoded_structural_size(in, ok);
    if (!ok) {
        return false;
    }
    const std::size_t n = in.size();
    std::size_t o = 0;
    // Padding is confined to the final quad (decoded_structural_size), so every
    // quad before it is four alphabet digits decoded without pad checks.
    std::size_t pad = 0;
    if (n > 0 && in[n - 1] == '=') {
        pad = in[n - 2] == '=' ? 2 : 1;
    }
    const std::size_t full_end = n - (pad > 0 ? 4 : 0);
    for (std::size_t i = 0; i < full_end; i += 4) {
        const std::uint8_t d0 = kDec.v[static_cast<unsigned char>(in[i])];
        const std::uint8_t d1 = kDec.v[static_cast<unsigned char>(in[i + 1])];
        const std::uint8_t d2 = kDec.v[static_cast<unsigned char>(in[i + 2])];
        const std::uint8_t d3 = kDec.v[static_cast<unsigned char>(in[i + 3])];
        if (((d0 | d1 | d2 | d3) & 0x80) != 0) {
            return false;
        }
        dst[o++] = static_cast<std::uint8_t>((d0 << 2) | (d1 >> 4));
        dst[o++] = static_cast<std::uint8_t>((d1 << 4) | (d2 >> 2));
        dst[o++] = static_cast<std::uint8_t>((d2 << 6) | d3);
    }
    if (pad > 0) {
        const std::uint8_t d0 = kDec.v[static_cast<unsigned char>(in[n - 4])];
        const std::uint8_t d1 = kDec.v[static_cast<unsigned char>(in[n - 3])];
        if (((d0 | d1) & 0x80) != 0) {
            return false;
        }
        dst[o++] = static_cast<std::uint8_t>((d0 << 2) | (d1 >> 4)); // "XX==" -> 1 byte
        if (pad == 2) {
            return o == out_len;
        }
        const std::uint8_t d2 = kDec.v[static_cast<unsigned char>(in[n - 2])];
        if ((d2 & 0x80) != 0) {
            return false;
        }
        dst[o++] = static_cast<std::uint8_t>((d1 << 4) | (d2 >> 2)); // "XXX=" -> 2 bytes
    }
    return o == out_len;
}

} // namespace fiber::util
