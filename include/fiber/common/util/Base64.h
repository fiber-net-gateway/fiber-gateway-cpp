#ifndef FIBER_UTIL_BASE64_H
#define FIBER_UTIL_BASE64_H

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fiber::util {

// RFC 4648 §4 base64 codec, matching java.util.Base64 basic encoder/decoder
// (alphabet A-Za-z0-9+/, '=' padding, no line breaks, strict decode).
//
// Huffman-style two-phase API (cf. src/http/Huffman.h): exact-size precompute +
// direct write, all noexcept and allocation-free, so noexcept callers (script
// host functions) can build into their own buffer without std::string staging.
// Callers wanting a std::string do resize(encoded_size) + _into themselves.

// Exact output length of base64_encode_into for len bytes. Performs no validation.
[[nodiscard]] constexpr std::size_t base64_encoded_size(std::size_t len) noexcept { return ((len + 2) / 3) * 4; }

// Structural validation (length % 4, '=' placement/count) plus the exact decoded
// byte count. ok=false covers everything a strict decoder rejects structurally;
// alphabet validity is checked during decode_into.
[[nodiscard]] std::size_t base64_decoded_size(std::string_view in, bool &ok) noexcept;

// Writes exactly base64_encoded_size(len) chars to dst. len == 0 writes nothing;
// dst may then be nullptr.
void base64_encode_into(const std::uint8_t *data, std::size_t len, char *dst) noexcept;

// Decodes into dst, which must hold exactly base64_decoded_size(in, ok=true)
// bytes. Returns false on any structural error or a character outside the
// alphabet (incl. whitespace/newlines), so it is safe standalone, not only after
// a decoded_size gate. Does not validate non-zero padding bits (matches Java's
// basic-decoder leniency there).
[[nodiscard]] bool base64_decode_into(std::string_view in, std::uint8_t *dst) noexcept;

} // namespace fiber::util

#endif // FIBER_UTIL_BASE64_H
