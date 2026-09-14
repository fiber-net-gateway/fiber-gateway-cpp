#ifndef FIBER_UTIL_CRC32_H
#define FIBER_UTIL_CRC32_H

// Standard CRC-32 (java.util.zip.CRC32 parity): reflected polynomial
// 0xEDB88320, init 0xFFFFFFFF, final XOR 0xFFFFFFFF. Empty input -> 0; the
// canonical check value "123456789" -> 0xCBF43926.
//
// The backend is the portable braided implementation from zlib 1.3.2 crc32.c
// (Mark Adler): N = 5 interleaved CRCs over 8- or 4-byte words with byte-wise
// head/tail handling, driven by static constexpr tables. This is CRC-32 (not
// CRC-32C); hardware acceleration must preserve that polynomial.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace fiber::util {

class Crc32 {
public:
    Crc32() noexcept = default;

    // Restart the computation from the initial state.
    void reset() noexcept { crc_ = 0xFFFFFFFFu; }

    // Continue the CRC with `data`. Safe on empty input.
    void update(std::span<const std::uint8_t> data) noexcept;
    void update(std::string_view data) noexcept {
        update(std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t *>(data.data()), data.size()});
    }

    // Current CRC-32 of everything updated so far.
    [[nodiscard]] std::uint32_t value() const noexcept { return crc_ ^ 0xFFFFFFFFu; }

    // One-shot CRC-32 of `data`.
    static std::uint32_t compute(std::span<const std::uint8_t> data) noexcept;
    static std::uint32_t compute(std::string_view data) noexcept;

private:
    std::uint32_t crc_ = 0xFFFFFFFFu;
};

} // namespace fiber::util

#endif // FIBER_UTIL_CRC32_H
