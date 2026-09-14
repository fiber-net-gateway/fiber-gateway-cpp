#ifndef FIBER_COMPRESSION_DEFLATE_TABLES_H
#define FIBER_COMPRESSION_DEFLATE_TABLES_H

// Static DEFLATE tables, derived from zlib 1.3.2 trees.h / the table
// generation logic in trees.c (Jean-loup Gailly; see src/compression/UPSTREAM.md).
// Everything is constexpr-generated here, mirroring the upstream generator
// (tr_static_init / gen_codes / bi_reverse), so the values match the tables
// shipped in the release archive without hand-transcribed constants.

#include <cstdint>

namespace fiber::compression {

// ---- code dimensions (zlib trees.h/deflate.h constants) ----
inline constexpr unsigned kDeflateLengthCodes = 29;
inline constexpr unsigned kDeflateLiterals = 256;
inline constexpr unsigned kDeflateLCodes = kDeflateLiterals + 1 + kDeflateLengthCodes; // 286
inline constexpr unsigned kDeflateDCodes = 30;
inline constexpr unsigned kDeflateBCodes = 19;
inline constexpr unsigned kDeflateHeapSize = 2 * kDeflateLCodes + 1; // 573
inline constexpr unsigned kDeflateMaxBits = 15;
inline constexpr unsigned kDeflateEndBlock = 256;
inline constexpr unsigned kDeflateMinMatch = 3;
inline constexpr unsigned kDeflateMaxMatch = 258;
inline constexpr unsigned kDeflateDistCodeLen = 512;

// Data structure describing a single value and its code string. During tree
// construction the fields hold frequency and parent node; when emitting a
// block they hold the bit code and its length (zlib ct_data).
struct DeflateCtData {
    union {
        std::uint16_t freq; // frequency count
        std::uint16_t code; // bit string
    };
    union {
        std::uint16_t dad; // father node in the Huffman tree
        std::uint16_t len; // length of the bit string
    };
};

struct DeflateStaticTreeDesc {
    const DeflateCtData *static_tree; // static tree or null
    const std::uint8_t *extra_bits; // extra bits for each code or null
    int extra_base; // base index for extra_bits
    int elems; // max number of elements in the tree
    int max_length; // max bit length for the codes
};

namespace deflate_tables {

// Reverse the first len bits of a code (zlib bi_reverse). 1 <= len <= 15.
constexpr unsigned bi_reverse(unsigned code, int len) noexcept {
    unsigned res = 0;
    do {
        res |= code & 1;
        code >>= 1;
        res <<= 1;
    } while (--len > 0);
    return res >> 1;
}

// Generate canonical codes for a tree from its bit counts (zlib gen_codes).
constexpr void gen_codes(DeflateCtData *tree, int max_code, std::uint16_t *bl_count) noexcept {
    std::uint16_t next_code[kDeflateMaxBits + 1] = {};
    unsigned code = 0;

    for (int bits = 1; bits <= static_cast<int>(kDeflateMaxBits); bits++) {
        code = (code + bl_count[bits - 1]) << 1;
        next_code[bits] = static_cast<std::uint16_t>(code);
    }

    for (int n = 0; n <= max_code; n++) {
        const int len = tree[n].len;
        if (len == 0) {
            continue;
        }
        tree[n].code = static_cast<std::uint16_t>(bi_reverse(next_code[len]++, len));
    }
}

struct StaticTables {
    // Static literal/length tree. Codes 286 and 287 do not exist but are
    // required to build a canonical tree.
    DeflateCtData static_ltree[kDeflateLCodes + 2] = {};
    // Static distance tree (all codes 5 bits).
    DeflateCtData static_dtree[kDeflateDCodes] = {};
    // Distance codes: the first 256 values cover distances 3..258, the last
    // 256 cover the top 8 bits of 15-bit distances.
    std::uint8_t dist_code[kDeflateDistCodeLen] = {};
    // Length code for each normalized match length (0 == MIN_MATCH).
    std::uint8_t length_code[kDeflateMaxMatch - kDeflateMinMatch + 1] = {};
    // First normalized length for each length code (0 = MIN_MATCH).
    std::uint16_t base_length[kDeflateLengthCodes] = {};
    // First normalized distance for each distance code (0 = distance of 1).
    std::uint16_t base_dist[kDeflateDCodes] = {};

    std::uint8_t extra_lbits[kDeflateLengthCodes] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                     2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    std::uint8_t extra_dbits[kDeflateDCodes] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    std::uint8_t extra_blbits[kDeflateBCodes] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 3, 7};
    // Bit length codes are sent in order of decreasing probability.
    std::uint8_t bl_order[kDeflateBCodes] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

    constexpr StaticTables() noexcept {
        // Mapping length (0..255) -> length code (0..28).
        int length = 0;
        for (int code = 0; code < static_cast<int>(kDeflateLengthCodes) - 1; code++) {
            base_length[code] = static_cast<std::uint16_t>(length);
            for (int n = 0; n < (1 << extra_lbits[code]); n++) {
                length_code[length++] = static_cast<std::uint8_t>(code);
            }
        }
        // Length 255 (match length 258) has two encodings; prefer code 285.
        length_code[length - 1] = static_cast<std::uint8_t>(kDeflateLengthCodes - 1);

        // Mapping dist (0..32K) -> dist code (0..29).
        int dist = 0;
        int code = 0;
        for (; code < 16; code++) {
            base_dist[code] = static_cast<std::uint16_t>(dist);
            for (int n = 0; n < (1 << extra_dbits[code]); n++) {
                dist_code[dist++] = static_cast<std::uint8_t>(code);
            }
        }
        dist >>= 7; // from now on, all distances are divided by 128
        for (; code < static_cast<int>(kDeflateDCodes); code++) {
            base_dist[code] = static_cast<std::uint16_t>(dist << 7);
            for (int n = 0; n < (1 << (extra_dbits[code] - 7)); n++) {
                dist_code[256 + dist++] = static_cast<std::uint8_t>(code);
            }
        }

        // Static literal tree bit lengths and canonical codes.
        std::uint16_t bl_count[kDeflateMaxBits + 1] = {};
        int n = 0;
        while (n <= 143) {
            static_ltree[n++].len = 8;
            bl_count[8]++;
        }
        while (n <= 255) {
            static_ltree[n++].len = 9;
            bl_count[9]++;
        }
        while (n <= 279) {
            static_ltree[n++].len = 7;
            bl_count[7]++;
        }
        while (n <= 287) {
            static_ltree[n++].len = 8;
            bl_count[8]++;
        }
        gen_codes(static_ltree, kDeflateLCodes + 1, bl_count);

        // The static distance tree is trivial.
        for (n = 0; n < static_cast<int>(kDeflateDCodes); n++) {
            static_dtree[n].len = 5;
            static_dtree[n].code = static_cast<std::uint16_t>(bi_reverse(n, 5));
        }
    }
};

inline constexpr StaticTables kTables{};

} // namespace deflate_tables

// Shorthand accessors mirroring the upstream symbol names.
inline constexpr auto &kDeflateStaticLTree = deflate_tables::kTables.static_ltree;
inline constexpr auto &kDeflateStaticDTree = deflate_tables::kTables.static_dtree;
inline constexpr auto &kDeflateDistCode = deflate_tables::kTables.dist_code;
inline constexpr auto &kDeflateLengthCode = deflate_tables::kTables.length_code;
inline constexpr auto &kDeflateBaseLength = deflate_tables::kTables.base_length;
inline constexpr auto &kDeflateBaseDist = deflate_tables::kTables.base_dist;
inline constexpr auto &kDeflateExtraLBits = deflate_tables::kTables.extra_lbits;
inline constexpr auto &kDeflateExtraDBits = deflate_tables::kTables.extra_dbits;
inline constexpr auto &kDeflateExtraBLBits = deflate_tables::kTables.extra_blbits;
inline constexpr auto &kDeflateBlOrder = deflate_tables::kTables.bl_order;

inline constexpr DeflateStaticTreeDesc kDeflateStaticLDesc{
        kDeflateStaticLTree, kDeflateExtraLBits, static_cast<int>(kDeflateLiterals) + 1,
        static_cast<int>(kDeflateLCodes), static_cast<int>(kDeflateMaxBits)};
inline constexpr DeflateStaticTreeDesc kDeflateStaticDDesc{kDeflateStaticDTree, kDeflateExtraDBits, 0,
                                                           static_cast<int>(kDeflateDCodes),
                                                           static_cast<int>(kDeflateMaxBits)};
inline constexpr DeflateStaticTreeDesc kDeflateStaticBLDesc{nullptr, kDeflateExtraBLBits, 0,
                                                            static_cast<int>(kDeflateBCodes), 7};

} // namespace fiber::compression

#endif // FIBER_COMPRESSION_DEFLATE_TABLES_H
