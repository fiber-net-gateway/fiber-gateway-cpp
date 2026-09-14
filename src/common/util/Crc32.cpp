#include "detail/Crc32Tables.h"

#include <fiber/common/util/Crc32.h>

#include <bit>
#include <cstddef>
#include <cstdint>

namespace fiber::util {

namespace {

using detail::Crc32Word;
using detail::kCrc32BigWordTable;
using detail::kCrc32Braids;
using detail::kCrc32BraidTables;
using detail::kCrc32ByteTable;
using detail::kCrc32WordSize;

// CRC of the W bytes in `data`, least-significant byte first, without pre or
// post conditioning (zlib crc_word; little-endian fold).
std::uint32_t crc_word_le(Crc32Word data) noexcept {
    for (std::size_t k = 0; k < kCrc32WordSize; ++k) {
        data = (data >> 8) ^ kCrc32ByteTable.entries[data & 0xff];
    }
    return static_cast<std::uint32_t>(data);
}

// Same fold for big-endian words (zlib crc_word_big).
Crc32Word crc_word_be(Crc32Word data) noexcept {
    for (std::size_t k = 0; k < kCrc32WordSize; ++k) {
        data = (data << 8) ^ kCrc32BigWordTable.entries[(data >> ((kCrc32WordSize - 1) << 3)) & 0xff];
    }
    return data;
}

constexpr Crc32Word swap_word(Crc32Word word) noexcept {
    if constexpr (kCrc32WordSize == 8) {
        return static_cast<Crc32Word>(__builtin_bswap64(static_cast<std::uint64_t>(word)));
    } else {
        return static_cast<Crc32Word>(__builtin_bswap32(static_cast<std::uint32_t>(word)));
    }
}

// Braided update (zlib crc32_z, N = 5): processes full words through five
// interleaved CRCs and folds them at the end. `buf`/`len` are advanced past
// the braided region. Requires len >= N*W + W - 1 on entry.
void crc32_braided(const std::uint8_t *&buf, std::size_t &len, std::uint32_t &crc) noexcept {
    // Compute the CRC up to a word boundary.
    while (len != 0 && (reinterpret_cast<std::uintptr_t>(buf) & (kCrc32WordSize - 1)) != 0) {
        --len;
        crc = (crc >> 8) ^ kCrc32ByteTable.entries[(crc ^ *buf++) & 0xff];
    }

    // Compute the CRC on as many N-word blocks as are available.
    std::size_t blks = len / (kCrc32Braids * kCrc32WordSize);
    len -= blks * kCrc32Braids * kCrc32WordSize;
    const Crc32Word *words = reinterpret_cast<const Crc32Word *>(buf);

    if constexpr (std::endian::native == std::endian::little) {
        // Initialize the CRC for each braid.
        std::uint32_t crc0 = crc;
        std::uint32_t crc1 = 0;
        std::uint32_t crc2 = 0;
        std::uint32_t crc3 = 0;
        std::uint32_t crc4 = 0;

        Crc32Word word0;
        Crc32Word word1;
        Crc32Word word2;
        Crc32Word word3;
        Crc32Word word4;

        // Process the first blks-1 blocks, computing the CRCs on each braid
        // independently.
        while (--blks != 0) {
            // Load the word for each braid into registers.
            word0 = crc0 ^ words[0];
            word1 = crc1 ^ words[1];
            word2 = crc2 ^ words[2];
            word3 = crc3 ^ words[3];
            word4 = crc4 ^ words[4];
            words += kCrc32Braids;

            // Compute and update the CRC for each word. The loop should get
            // unrolled.
            crc0 = kCrc32BraidTables.little[0][word0 & 0xff];
            crc1 = kCrc32BraidTables.little[0][word1 & 0xff];
            crc2 = kCrc32BraidTables.little[0][word2 & 0xff];
            crc3 = kCrc32BraidTables.little[0][word3 & 0xff];
            crc4 = kCrc32BraidTables.little[0][word4 & 0xff];
            for (std::size_t k = 1; k < kCrc32WordSize; ++k) {
                crc0 ^= kCrc32BraidTables.little[k][(word0 >> (k << 3)) & 0xff];
                crc1 ^= kCrc32BraidTables.little[k][(word1 >> (k << 3)) & 0xff];
                crc2 ^= kCrc32BraidTables.little[k][(word2 >> (k << 3)) & 0xff];
                crc3 ^= kCrc32BraidTables.little[k][(word3 >> (k << 3)) & 0xff];
                crc4 ^= kCrc32BraidTables.little[k][(word4 >> (k << 3)) & 0xff];
            }
        }

        // Process the last block, combining the CRCs of the five braids at
        // the same time.
        crc = crc_word_le(crc0 ^ words[0]);
        crc = crc_word_le(crc1 ^ words[1] ^ crc);
        crc = crc_word_le(crc2 ^ words[2] ^ crc);
        crc = crc_word_le(crc3 ^ words[3] ^ crc);
        crc = crc_word_le(crc4 ^ words[4] ^ crc);
        words += kCrc32Braids;
    } else {
        // Big endian.
        Crc32Word crc0 = swap_word(crc);
        Crc32Word crc1 = 0;
        Crc32Word crc2 = 0;
        Crc32Word crc3 = 0;
        Crc32Word crc4 = 0;

        Crc32Word word0;
        Crc32Word word1;
        Crc32Word word2;
        Crc32Word word3;
        Crc32Word word4;
        Crc32Word comb;

        while (--blks != 0) {
            word0 = crc0 ^ words[0];
            word1 = crc1 ^ words[1];
            word2 = crc2 ^ words[2];
            word3 = crc3 ^ words[3];
            word4 = crc4 ^ words[4];
            words += kCrc32Braids;

            crc0 = kCrc32BraidTables.big[0][word0 & 0xff];
            crc1 = kCrc32BraidTables.big[0][word1 & 0xff];
            crc2 = kCrc32BraidTables.big[0][word2 & 0xff];
            crc3 = kCrc32BraidTables.big[0][word3 & 0xff];
            crc4 = kCrc32BraidTables.big[0][word4 & 0xff];
            for (std::size_t k = 1; k < kCrc32WordSize; ++k) {
                crc0 ^= kCrc32BraidTables.big[k][(word0 >> (k << 3)) & 0xff];
                crc1 ^= kCrc32BraidTables.big[k][(word1 >> (k << 3)) & 0xff];
                crc2 ^= kCrc32BraidTables.big[k][(word2 >> (k << 3)) & 0xff];
                crc3 ^= kCrc32BraidTables.big[k][(word3 >> (k << 3)) & 0xff];
                crc4 ^= kCrc32BraidTables.big[k][(word4 >> (k << 3)) & 0xff];
            }
        }

        comb = crc_word_be(crc0 ^ words[0]);
        comb = crc_word_be(crc1 ^ words[1] ^ comb);
        comb = crc_word_be(crc2 ^ words[2] ^ comb);
        comb = crc_word_be(crc3 ^ words[3] ^ comb);
        comb = crc_word_be(crc4 ^ words[4] ^ comb);
        words += kCrc32Braids;
        crc = static_cast<std::uint32_t>(swap_word(comb));
    }

    // Update the pointer to the remaining bytes to process.
    buf = reinterpret_cast<const std::uint8_t *>(words);
}

std::uint32_t crc32_update(std::uint32_t crc, const std::uint8_t *buf, std::size_t len) noexcept {
    // If provided enough bytes, do a braided CRC calculation.
    if (len >= kCrc32Braids * kCrc32WordSize + kCrc32WordSize - 1) {
        crc32_braided(buf, len, crc);
    }

    // Complete the computation of the CRC on any remaining bytes.
    while (len >= 8) {
        len -= 8;
        crc = (crc >> 8) ^ kCrc32ByteTable.entries[(crc ^ *buf++) & 0xff];
        crc = (crc >> 8) ^ kCrc32ByteTable.entries[(crc ^ *buf++) & 0xff];
        crc = (crc >> 8) ^ kCrc32ByteTable.entries[(crc ^ *buf++) & 0xff];
        crc = (crc >> 8) ^ kCrc32ByteTable.entries[(crc ^ *buf++) & 0xff];
        crc = (crc >> 8) ^ kCrc32ByteTable.entries[(crc ^ *buf++) & 0xff];
        crc = (crc >> 8) ^ kCrc32ByteTable.entries[(crc ^ *buf++) & 0xff];
        crc = (crc >> 8) ^ kCrc32ByteTable.entries[(crc ^ *buf++) & 0xff];
        crc = (crc >> 8) ^ kCrc32ByteTable.entries[(crc ^ *buf++) & 0xff];
    }
    while (len != 0) {
        --len;
        crc = (crc >> 8) ^ kCrc32ByteTable.entries[(crc ^ *buf++) & 0xff];
    }
    return crc;
}

} // namespace

void Crc32::update(std::span<const std::uint8_t> data) noexcept { crc_ = crc32_update(crc_, data.data(), data.size()); }

std::uint32_t Crc32::compute(std::span<const std::uint8_t> data) noexcept {
    return crc32_update(0xFFFFFFFFu, data.data(), data.size()) ^ 0xFFFFFFFFu;
}

std::uint32_t Crc32::compute(std::string_view data) noexcept {
    return compute(std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t *>(data.data()), data.size()});
}

} // namespace fiber::util
