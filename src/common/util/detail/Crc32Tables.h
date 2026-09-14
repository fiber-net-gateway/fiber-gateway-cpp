#ifndef FIBER_UTIL_DETAIL_CRC32_TABLES_H
#define FIBER_UTIL_DETAIL_CRC32_TABLES_H

// Static CRC-32 tables derived from zlib 1.3.2 crc32.c (Mark Adler). The
// generation logic mirrors make_crc_table()/braid() with the default braid
// width N = 5; the values equal the tables shipped in upstream crc32.h. All
// tables are constexpr so there is no first-use initialization cost.

#include <array>
#include <cstddef>
#include <cstdint>

namespace fiber::util::detail {

// zlib 1.3.2 picks an eight-byte braid on the 64-bit targets it benchmarked
// and a four-byte braid elsewhere (crc32.c W selection). Mirror that choice so
// the table footprint and throughput profile stay comparable. The macro keeps
// the word size usable in preprocessor conditions and array shapes.
#if defined(__x86_64__) || defined(__aarch64__)
#define FIBER_CRC32_WORD_BYTES 8
#else
#define FIBER_CRC32_WORD_BYTES 4
#endif

inline constexpr std::size_t kCrc32WordSize = FIBER_CRC32_WORD_BYTES;

#if FIBER_CRC32_WORD_BYTES == 8
using Crc32Word = std::uint64_t;
#else
using Crc32Word = std::uint32_t;
#endif

inline constexpr std::size_t kCrc32Braids = 5;
inline constexpr std::uint32_t kCrc32Poly = 0xedb88320u;

// Byte-wise table: CRC of each possible byte, poly 0xedb88320 reflected.
struct Crc32ByteTable {
    std::uint32_t entries[256]{};

    constexpr Crc32ByteTable() noexcept {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) != 0u ? kCrc32Poly ^ (c >> 1) : c >> 1;
            }
            entries[i] = c;
        }
    }
};

inline constexpr Crc32ByteTable kCrc32ByteTable{};

// ---- polynomial helpers used to derive the braid tables (zlib multmodp /
// x2nmodp), kept constexpr for table generation only ----

// a(x) * b(x) modulo p(x), reflected; requires a != 0.
constexpr std::uint64_t crc32_multmodp(std::uint64_t a, std::uint64_t b) noexcept {
    std::uint64_t m = std::uint64_t{1} << 31;
    std::uint64_t p = 0;
    for (;;) {
        if ((a & m) != 0) {
            p ^= b;
            if ((a & (m - 1)) == 0) {
                break;
            }
        }
        m >>= 1;
        b = (b & 1) != 0 ? (b >> 1) ^ kCrc32Poly : b >> 1;
    }
    return p;
}

constexpr std::array<std::uint64_t, 32> make_crc32_x2n_table() noexcept {
    std::array<std::uint64_t, 32> table{};
    std::uint64_t p = std::uint64_t{1} << 30; // x^1
    table[0] = p;
    for (std::size_t n = 1; n < 32; ++n) {
        p = crc32_multmodp(p, p);
        table[n] = p;
    }
    return table;
}

inline constexpr std::array<std::uint64_t, 32> kCrc32X2nTable = make_crc32_x2n_table();

// x^(n * 2^k) modulo p(x).
constexpr std::uint64_t crc32_x2nmodp(std::uint64_t n, unsigned k) noexcept {
    std::uint64_t p = std::uint64_t{1} << 31; // x^0 == 1
    while (n != 0) {
        if ((n & 1) != 0) {
            p = crc32_multmodp(kCrc32X2nTable[k & 31], p);
        }
        n >>= 1;
        ++k;
    }
    return p;
}

constexpr Crc32Word crc32_swap_word(std::uint32_t value) noexcept {
    if constexpr (kCrc32WordSize == 8) {
        // Byte-swap of the zero-extended 64-bit word (upstream byte_swap, W=8).
        std::uint64_t v = value;
        v = ((v & 0xff00000000000000) >> 56) | ((v & 0xff000000000000) >> 40) | ((v & 0xff0000000000) >> 24) |
            ((v & 0xff00000000) >> 8) | ((v & 0xff000000) << 8) | ((v & 0xff0000) << 24) | ((v & 0xff00) << 40) |
            ((v & 0xff) << 56);
        return v;
    } else {
        std::uint32_t v = value;
        v = ((v & 0xff000000) >> 24) | ((v & 0xff0000) >> 8) | ((v & 0xff00) << 8) | ((v & 0xff) << 24);
        return v;
    }
}

// Little-endian and big-endian braid tables (zlib crc_braid_table /
// crc_braid_big_table). Upstream shapes them [W][256]: one row per byte
// position within the word. The runtime braid count N = 5 is independent of
// the row count.
struct Crc32BraidTables {
    std::uint32_t little[kCrc32WordSize][256]{};
    Crc32Word big[kCrc32WordSize][256]{};

    constexpr Crc32BraidTables() noexcept {
        constexpr std::size_t n = kCrc32Braids;
        constexpr std::size_t w = kCrc32WordSize;
        for (std::size_t k = 0; k < w; ++k) {
            const std::uint64_t p = crc32_x2nmodp((n * w + 3 - k) << 3, 0);
            little[k][0] = 0;
            big[w - 1 - k][0] = 0;
            for (std::uint32_t i = 1; i < 256; ++i) {
                const auto q = static_cast<std::uint32_t>(crc32_multmodp(static_cast<std::uint64_t>(i) << 24, p));
                little[k][i] = q;
                big[w - 1 - k][i] = crc32_swap_word(q);
            }
        }
    }
};

inline constexpr Crc32BraidTables kCrc32BraidTables{};

// crc_big_table: byte-swapped byte-wise table, used to fold a processed big-
// endian word back into the CRC (upstream crc_word_big).
struct Crc32BigWordTable {
    Crc32Word entries[256]{};

    constexpr Crc32BigWordTable() noexcept {
        for (std::uint32_t i = 0; i < 256; ++i) {
            entries[i] = crc32_swap_word(kCrc32ByteTable.entries[i]);
        }
    }
};

inline constexpr Crc32BigWordTable kCrc32BigWordTable{};

} // namespace fiber::util::detail

#endif // FIBER_UTIL_DETAIL_CRC32_TABLES_H
