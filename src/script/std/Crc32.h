#ifndef FIBER_SCRIPT_STD_CRC32_H
#define FIBER_SCRIPT_STD_CRC32_H

#include <cstddef>
#include <cstdint>

namespace fiber::script::std_lib {

// Standard CRC-32 (java.util.zip.CRC32 parity): reflected poly 0xEDB88320, init
// 0xFFFFFFFF, final XOR 0xFFFFFFFF. Empty input -> 0; the canonical check value
// "123456789" -> 0xCBF43926. Internal helper shared by RandFuncs.canary and
// HashFuncs.crc32; not a registered script function.

constexpr std::uint32_t kCrc32Poly = 0xEDB88320u;

constexpr std::uint32_t crc32_table_entry(std::uint32_t index) noexcept {
    std::uint32_t c = index;
    for (int k = 0; k < 8; ++k) {
        c = (c & 1u) != 0u ? (kCrc32Poly ^ (c >> 1)) : (c >> 1);
    }
    return c;
}

struct Crc32Table {
    std::uint32_t entries[256]{};
    constexpr Crc32Table() noexcept {
        for (std::uint32_t i = 0; i < 256u; ++i) {
            entries[i] = crc32_table_entry(i);
        }
    }
};

inline constexpr Crc32Table kCrc32Table;

// Continues the CRC-32 in |crc| (which starts at 0xFFFFFFFF) with [data, data+len).
inline void crc32_update(std::uint32_t &crc, const char *data, std::size_t len) noexcept {
    for (std::size_t i = 0; i < len; ++i) {
        std::uint32_t idx = (crc ^ static_cast<std::uint8_t>(data[i])) & 0xFFu;
        crc = kCrc32Table.entries[idx] ^ (crc >> 8);
    }
}

// One-shot CRC-32 of [data, data+len) (init + update + final XOR).
inline std::uint32_t crc32_bytes(const char *data, std::size_t len) noexcept {
    std::uint32_t crc = 0xFFFFFFFFu;
    crc32_update(crc, data, len);
    return crc ^ 0xFFFFFFFFu;
}

} // namespace fiber::script::std_lib

#endif // FIBER_SCRIPT_STD_CRC32_H
