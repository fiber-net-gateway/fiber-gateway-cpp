#ifndef FIBER_COMMON_BASE64_BASE64_H
#define FIBER_COMMON_BASE64_BASE64_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace fiber::common::base64 {

// RFC 4648 §4 base64 encode: alphabet A-Za-z0-9+/, '=' padding, no line breaks.
// Matches java.util.Base64.getEncoder() (basic encoder).
std::string base64_encode(const std::uint8_t *data, std::size_t len);

// Strict basic base64 decode (java.util.Base64.getDecoder() parity). Returns false
// on any structural error: length not a multiple of 4, characters outside the
// alphabet (incl. whitespace/newlines), or misplaced/overlong '=' padding. On
// success, writes decoded bytes to `out` (cleared first). Does not validate
// non-zero padding bits (matches Java's basic-decoder leniency there).
bool base64_decode(std::string_view in, std::string &out) noexcept;

} // namespace fiber::common::base64

#endif // FIBER_COMMON_BASE64_BASE64_H
