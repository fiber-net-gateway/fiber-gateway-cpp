#include "http/Huffman.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace fiber::http {
namespace {

struct EncodeCode {
    std::uint32_t bits;
    std::uint8_t bit_length;
};

struct TrieNode {
    std::uint16_t child0;
    std::uint16_t child1;
    std::uint16_t symbol;
};

struct PackedRoot16Entry {
    std::uint64_t value;
};

struct PackedByteEntry {
    std::uint32_t value;
};

struct Root16DecodeTable {
    std::array<PackedRoot16Entry, 65536> entries{};
    bool output_overflow = false;
};

struct ByteDecodeRow {
    std::array<PackedByteEntry, 256> entries{};
    bool output_overflow = false;
};

struct ByteDecodeTables {
    std::array<const ByteDecodeRow *, 256> rows{};
    std::uint16_t state_count = 0;
    bool state_overflow = false;
    bool output_overflow = false;
};

struct DecodeBuildTables {
    std::array<TrieNode, 1024> trie{};
    std::array<bool, 1024> ending{};
    std::array<std::uint16_t, 1024> trie_to_state{};
    std::array<std::uint16_t, 256> state_to_trie{};
    std::array<std::uint16_t, 256> queue{};
    std::uint16_t trie_size = 1;
    std::uint16_t state_count = 0;
    bool state_overflow = false;
};

struct DecodeTransition {
    std::uint16_t next_state = 0;
    std::uint8_t emit_count = 0;
    std::uint8_t out[3]{};
    bool accepted = false;
    bool invalid = false;
    bool output_overflow = false;
};

constexpr std::uint16_t kInvalidNode = 0xffffU;
constexpr std::uint16_t kNoSymbol = 0xffffU;
constexpr std::uint16_t kEosSymbol = 256U;
constexpr std::uint16_t kInvalidState = 0xffffU;

constexpr std::array<EncodeCode, 257> kCodeTable{{
        {.bits = 0x00001ff8, .bit_length = 13}, {.bits = 0x007fffd8, .bit_length = 23},
        {.bits = 0x0fffffe2, .bit_length = 28}, {.bits = 0x0fffffe3, .bit_length = 28},
        {.bits = 0x0fffffe4, .bit_length = 28}, {.bits = 0x0fffffe5, .bit_length = 28},
        {.bits = 0x0fffffe6, .bit_length = 28}, {.bits = 0x0fffffe7, .bit_length = 28},
        {.bits = 0x0fffffe8, .bit_length = 28}, {.bits = 0x00ffffea, .bit_length = 24},
        {.bits = 0x3ffffffc, .bit_length = 30}, {.bits = 0x0fffffe9, .bit_length = 28},
        {.bits = 0x0fffffea, .bit_length = 28}, {.bits = 0x3ffffffd, .bit_length = 30},
        {.bits = 0x0fffffeb, .bit_length = 28}, {.bits = 0x0fffffec, .bit_length = 28},
        {.bits = 0x0fffffed, .bit_length = 28}, {.bits = 0x0fffffee, .bit_length = 28},
        {.bits = 0x0fffffef, .bit_length = 28}, {.bits = 0x0ffffff0, .bit_length = 28},
        {.bits = 0x0ffffff1, .bit_length = 28}, {.bits = 0x0ffffff2, .bit_length = 28},
        {.bits = 0x3ffffffe, .bit_length = 30}, {.bits = 0x0ffffff3, .bit_length = 28},
        {.bits = 0x0ffffff4, .bit_length = 28}, {.bits = 0x0ffffff5, .bit_length = 28},
        {.bits = 0x0ffffff6, .bit_length = 28}, {.bits = 0x0ffffff7, .bit_length = 28},
        {.bits = 0x0ffffff8, .bit_length = 28}, {.bits = 0x0ffffff9, .bit_length = 28},
        {.bits = 0x0ffffffa, .bit_length = 28}, {.bits = 0x0ffffffb, .bit_length = 28},
        {.bits = 0x00000014, .bit_length = 6},  {.bits = 0x000003f8, .bit_length = 10},
        {.bits = 0x000003f9, .bit_length = 10}, {.bits = 0x00000ffa, .bit_length = 12},
        {.bits = 0x00001ff9, .bit_length = 13}, {.bits = 0x00000015, .bit_length = 6},
        {.bits = 0x000000f8, .bit_length = 8},  {.bits = 0x000007fa, .bit_length = 11},
        {.bits = 0x000003fa, .bit_length = 10}, {.bits = 0x000003fb, .bit_length = 10},
        {.bits = 0x000000f9, .bit_length = 8},  {.bits = 0x000007fb, .bit_length = 11},
        {.bits = 0x000000fa, .bit_length = 8},  {.bits = 0x00000016, .bit_length = 6},
        {.bits = 0x00000017, .bit_length = 6},  {.bits = 0x00000018, .bit_length = 6},
        {.bits = 0x00000000, .bit_length = 5},  {.bits = 0x00000001, .bit_length = 5},
        {.bits = 0x00000002, .bit_length = 5},  {.bits = 0x00000019, .bit_length = 6},
        {.bits = 0x0000001a, .bit_length = 6},  {.bits = 0x0000001b, .bit_length = 6},
        {.bits = 0x0000001c, .bit_length = 6},  {.bits = 0x0000001d, .bit_length = 6},
        {.bits = 0x0000001e, .bit_length = 6},  {.bits = 0x0000001f, .bit_length = 6},
        {.bits = 0x0000005c, .bit_length = 7},  {.bits = 0x000000fb, .bit_length = 8},
        {.bits = 0x00007ffc, .bit_length = 15}, {.bits = 0x00000020, .bit_length = 6},
        {.bits = 0x00000ffb, .bit_length = 12}, {.bits = 0x000003fc, .bit_length = 10},
        {.bits = 0x00001ffa, .bit_length = 13}, {.bits = 0x00000021, .bit_length = 6},
        {.bits = 0x0000005d, .bit_length = 7},  {.bits = 0x0000005e, .bit_length = 7},
        {.bits = 0x0000005f, .bit_length = 7},  {.bits = 0x00000060, .bit_length = 7},
        {.bits = 0x00000061, .bit_length = 7},  {.bits = 0x00000062, .bit_length = 7},
        {.bits = 0x00000063, .bit_length = 7},  {.bits = 0x00000064, .bit_length = 7},
        {.bits = 0x00000065, .bit_length = 7},  {.bits = 0x00000066, .bit_length = 7},
        {.bits = 0x00000067, .bit_length = 7},  {.bits = 0x00000068, .bit_length = 7},
        {.bits = 0x00000069, .bit_length = 7},  {.bits = 0x0000006a, .bit_length = 7},
        {.bits = 0x0000006b, .bit_length = 7},  {.bits = 0x0000006c, .bit_length = 7},
        {.bits = 0x0000006d, .bit_length = 7},  {.bits = 0x0000006e, .bit_length = 7},
        {.bits = 0x0000006f, .bit_length = 7},  {.bits = 0x00000070, .bit_length = 7},
        {.bits = 0x00000071, .bit_length = 7},  {.bits = 0x00000072, .bit_length = 7},
        {.bits = 0x000000fc, .bit_length = 8},  {.bits = 0x00000073, .bit_length = 7},
        {.bits = 0x000000fd, .bit_length = 8},  {.bits = 0x00001ffb, .bit_length = 13},
        {.bits = 0x0007fff0, .bit_length = 19}, {.bits = 0x00001ffc, .bit_length = 13},
        {.bits = 0x00003ffc, .bit_length = 14}, {.bits = 0x00000022, .bit_length = 6},
        {.bits = 0x00007ffd, .bit_length = 15}, {.bits = 0x00000003, .bit_length = 5},
        {.bits = 0x00000023, .bit_length = 6},  {.bits = 0x00000004, .bit_length = 5},
        {.bits = 0x00000024, .bit_length = 6},  {.bits = 0x00000005, .bit_length = 5},
        {.bits = 0x00000025, .bit_length = 6},  {.bits = 0x00000026, .bit_length = 6},
        {.bits = 0x00000027, .bit_length = 6},  {.bits = 0x00000006, .bit_length = 5},
        {.bits = 0x00000074, .bit_length = 7},  {.bits = 0x00000075, .bit_length = 7},
        {.bits = 0x00000028, .bit_length = 6},  {.bits = 0x00000029, .bit_length = 6},
        {.bits = 0x0000002a, .bit_length = 6},  {.bits = 0x00000007, .bit_length = 5},
        {.bits = 0x0000002b, .bit_length = 6},  {.bits = 0x00000076, .bit_length = 7},
        {.bits = 0x0000002c, .bit_length = 6},  {.bits = 0x00000008, .bit_length = 5},
        {.bits = 0x00000009, .bit_length = 5},  {.bits = 0x0000002d, .bit_length = 6},
        {.bits = 0x00000077, .bit_length = 7},  {.bits = 0x00000078, .bit_length = 7},
        {.bits = 0x00000079, .bit_length = 7},  {.bits = 0x0000007a, .bit_length = 7},
        {.bits = 0x0000007b, .bit_length = 7},  {.bits = 0x00007ffe, .bit_length = 15},
        {.bits = 0x000007fc, .bit_length = 11}, {.bits = 0x00003ffd, .bit_length = 14},
        {.bits = 0x00001ffd, .bit_length = 13}, {.bits = 0x0ffffffc, .bit_length = 28},
        {.bits = 0x000fffe6, .bit_length = 20}, {.bits = 0x003fffd2, .bit_length = 22},
        {.bits = 0x000fffe7, .bit_length = 20}, {.bits = 0x000fffe8, .bit_length = 20},
        {.bits = 0x003fffd3, .bit_length = 22}, {.bits = 0x003fffd4, .bit_length = 22},
        {.bits = 0x003fffd5, .bit_length = 22}, {.bits = 0x007fffd9, .bit_length = 23},
        {.bits = 0x003fffd6, .bit_length = 22}, {.bits = 0x007fffda, .bit_length = 23},
        {.bits = 0x007fffdb, .bit_length = 23}, {.bits = 0x007fffdc, .bit_length = 23},
        {.bits = 0x007fffdd, .bit_length = 23}, {.bits = 0x007fffde, .bit_length = 23},
        {.bits = 0x00ffffeb, .bit_length = 24}, {.bits = 0x007fffdf, .bit_length = 23},
        {.bits = 0x00ffffec, .bit_length = 24}, {.bits = 0x00ffffed, .bit_length = 24},
        {.bits = 0x003fffd7, .bit_length = 22}, {.bits = 0x007fffe0, .bit_length = 23},
        {.bits = 0x00ffffee, .bit_length = 24}, {.bits = 0x007fffe1, .bit_length = 23},
        {.bits = 0x007fffe2, .bit_length = 23}, {.bits = 0x007fffe3, .bit_length = 23},
        {.bits = 0x007fffe4, .bit_length = 23}, {.bits = 0x001fffdc, .bit_length = 21},
        {.bits = 0x003fffd8, .bit_length = 22}, {.bits = 0x007fffe5, .bit_length = 23},
        {.bits = 0x003fffd9, .bit_length = 22}, {.bits = 0x007fffe6, .bit_length = 23},
        {.bits = 0x007fffe7, .bit_length = 23}, {.bits = 0x00ffffef, .bit_length = 24},
        {.bits = 0x003fffda, .bit_length = 22}, {.bits = 0x001fffdd, .bit_length = 21},
        {.bits = 0x000fffe9, .bit_length = 20}, {.bits = 0x003fffdb, .bit_length = 22},
        {.bits = 0x003fffdc, .bit_length = 22}, {.bits = 0x007fffe8, .bit_length = 23},
        {.bits = 0x007fffe9, .bit_length = 23}, {.bits = 0x001fffde, .bit_length = 21},
        {.bits = 0x007fffea, .bit_length = 23}, {.bits = 0x003fffdd, .bit_length = 22},
        {.bits = 0x003fffde, .bit_length = 22}, {.bits = 0x00fffff0, .bit_length = 24},
        {.bits = 0x001fffdf, .bit_length = 21}, {.bits = 0x003fffdf, .bit_length = 22},
        {.bits = 0x007fffeb, .bit_length = 23}, {.bits = 0x007fffec, .bit_length = 23},
        {.bits = 0x001fffe0, .bit_length = 21}, {.bits = 0x001fffe1, .bit_length = 21},
        {.bits = 0x003fffe0, .bit_length = 22}, {.bits = 0x001fffe2, .bit_length = 21},
        {.bits = 0x007fffed, .bit_length = 23}, {.bits = 0x003fffe1, .bit_length = 22},
        {.bits = 0x007fffee, .bit_length = 23}, {.bits = 0x007fffef, .bit_length = 23},
        {.bits = 0x000fffea, .bit_length = 20}, {.bits = 0x003fffe2, .bit_length = 22},
        {.bits = 0x003fffe3, .bit_length = 22}, {.bits = 0x003fffe4, .bit_length = 22},
        {.bits = 0x007ffff0, .bit_length = 23}, {.bits = 0x003fffe5, .bit_length = 22},
        {.bits = 0x003fffe6, .bit_length = 22}, {.bits = 0x007ffff1, .bit_length = 23},
        {.bits = 0x03ffffe0, .bit_length = 26}, {.bits = 0x03ffffe1, .bit_length = 26},
        {.bits = 0x000fffeb, .bit_length = 20}, {.bits = 0x0007fff1, .bit_length = 19},
        {.bits = 0x003fffe7, .bit_length = 22}, {.bits = 0x007ffff2, .bit_length = 23},
        {.bits = 0x003fffe8, .bit_length = 22}, {.bits = 0x01ffffec, .bit_length = 25},
        {.bits = 0x03ffffe2, .bit_length = 26}, {.bits = 0x03ffffe3, .bit_length = 26},
        {.bits = 0x03ffffe4, .bit_length = 26}, {.bits = 0x07ffffde, .bit_length = 27},
        {.bits = 0x07ffffdf, .bit_length = 27}, {.bits = 0x03ffffe5, .bit_length = 26},
        {.bits = 0x00fffff1, .bit_length = 24}, {.bits = 0x01ffffed, .bit_length = 25},
        {.bits = 0x0007fff2, .bit_length = 19}, {.bits = 0x001fffe3, .bit_length = 21},
        {.bits = 0x03ffffe6, .bit_length = 26}, {.bits = 0x07ffffe0, .bit_length = 27},
        {.bits = 0x07ffffe1, .bit_length = 27}, {.bits = 0x03ffffe7, .bit_length = 26},
        {.bits = 0x07ffffe2, .bit_length = 27}, {.bits = 0x00fffff2, .bit_length = 24},
        {.bits = 0x001fffe4, .bit_length = 21}, {.bits = 0x001fffe5, .bit_length = 21},
        {.bits = 0x03ffffe8, .bit_length = 26}, {.bits = 0x03ffffe9, .bit_length = 26},
        {.bits = 0x0ffffffd, .bit_length = 28}, {.bits = 0x07ffffe3, .bit_length = 27},
        {.bits = 0x07ffffe4, .bit_length = 27}, {.bits = 0x07ffffe5, .bit_length = 27},
        {.bits = 0x000fffec, .bit_length = 20}, {.bits = 0x00fffff3, .bit_length = 24},
        {.bits = 0x000fffed, .bit_length = 20}, {.bits = 0x001fffe6, .bit_length = 21},
        {.bits = 0x003fffe9, .bit_length = 22}, {.bits = 0x001fffe7, .bit_length = 21},
        {.bits = 0x001fffe8, .bit_length = 21}, {.bits = 0x007ffff3, .bit_length = 23},
        {.bits = 0x003fffea, .bit_length = 22}, {.bits = 0x003fffeb, .bit_length = 22},
        {.bits = 0x01ffffee, .bit_length = 25}, {.bits = 0x01ffffef, .bit_length = 25},
        {.bits = 0x00fffff4, .bit_length = 24}, {.bits = 0x00fffff5, .bit_length = 24},
        {.bits = 0x03ffffea, .bit_length = 26}, {.bits = 0x007ffff4, .bit_length = 23},
        {.bits = 0x03ffffeb, .bit_length = 26}, {.bits = 0x07ffffe6, .bit_length = 27},
        {.bits = 0x03ffffec, .bit_length = 26}, {.bits = 0x03ffffed, .bit_length = 26},
        {.bits = 0x07ffffe7, .bit_length = 27}, {.bits = 0x07ffffe8, .bit_length = 27},
        {.bits = 0x07ffffe9, .bit_length = 27}, {.bits = 0x07ffffea, .bit_length = 27},
        {.bits = 0x07ffffeb, .bit_length = 27}, {.bits = 0x0ffffffe, .bit_length = 28},
        {.bits = 0x07ffffec, .bit_length = 27}, {.bits = 0x07ffffed, .bit_length = 27},
        {.bits = 0x07ffffee, .bit_length = 27}, {.bits = 0x07ffffef, .bit_length = 27},
        {.bits = 0x07fffff0, .bit_length = 27}, {.bits = 0x03ffffee, .bit_length = 26},
        {.bits = 0x3fffffff, .bit_length = 30},
}};

constexpr TrieNode make_trie_node() noexcept {
    return {.child0 = kInvalidNode, .child1 = kInvalidNode, .symbol = kNoSymbol};
}

constexpr std::uint8_t to_lower_ascii(std::uint8_t ch) noexcept {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<std::uint8_t>(ch + ('a' - 'A'));
    }
    return ch;
}

constexpr std::array<EncodeCode, 256> build_ascii_lower_code_table() noexcept {
    std::array<EncodeCode, 256> table{};
    for (std::size_t i = 0; i < table.size(); ++i) {
        table[i] = kCodeTable[to_lower_ascii(static_cast<std::uint8_t>(i))];
    }
    return table;
}

constexpr std::array<EncodeCode, 256> kAsciiLowerCodeTable = build_ascii_lower_code_table();

static_assert(kAsciiLowerCodeTable['A'].bits == kCodeTable['a'].bits &&
                      kAsciiLowerCodeTable['A'].bit_length == kCodeTable['a'].bit_length,
              "ASCII-lower HPACK Huffman encode table must pre-lowercase uppercase letters");

constexpr const EncodeCode *select_encode_code_table(HpackHuffmanLowerMode lower_mode) noexcept {
    return lower_mode == HpackHuffmanLowerMode::Ascii ? kAsciiLowerCodeTable.data() : kCodeTable.data();
}

constexpr std::uint64_t keep_low_bits(std::uint64_t bits, std::uint8_t bit_count) noexcept {
    if (bit_count == 0) {
        return 0;
    }
    return bits & ((std::uint64_t{1} << bit_count) - 1U);
}

constexpr PackedByteEntry pack_byte_entry(DecodeTransition transition) noexcept {
    return {static_cast<std::uint32_t>(transition.out[0]) | (static_cast<std::uint32_t>(transition.out[1]) << 8U) |
            (static_cast<std::uint32_t>(transition.next_state) << 16U) |
            (static_cast<std::uint32_t>(transition.emit_count) << 26U) |
            (static_cast<std::uint32_t>(transition.accepted) << 28U) |
            (static_cast<std::uint32_t>(transition.invalid) << 29U)};
}

constexpr PackedRoot16Entry pack_root16_entry(DecodeTransition transition) noexcept {
    return {static_cast<std::uint64_t>(transition.out[0]) | (static_cast<std::uint64_t>(transition.out[1]) << 8U) |
            (static_cast<std::uint64_t>(transition.out[2]) << 16U) |
            (static_cast<std::uint64_t>(transition.next_state) << 24U) |
            (static_cast<std::uint64_t>(transition.emit_count) << 34U) |
            (static_cast<std::uint64_t>(transition.accepted) << 36U) |
            (static_cast<std::uint64_t>(transition.invalid) << 37U)};
}

constexpr std::uint8_t byte_emit_count(PackedByteEntry entry) noexcept {
    return static_cast<std::uint8_t>((entry.value >> 26U) & 0x03U);
}

constexpr std::uint8_t root16_emit_count(PackedRoot16Entry entry) noexcept {
    return static_cast<std::uint8_t>((entry.value >> 34U) & 0x03U);
}

constexpr std::uint16_t byte_next_state(PackedByteEntry entry) noexcept {
    return static_cast<std::uint16_t>((entry.value >> 16U) & 0x03ffU);
}

constexpr std::uint16_t root16_next_state(PackedRoot16Entry entry) noexcept {
    return static_cast<std::uint16_t>((entry.value >> 24U) & 0x03ffU);
}

constexpr bool byte_accepted(PackedByteEntry entry) noexcept { return ((entry.value >> 28U) & 0x01U) != 0; }

constexpr bool root16_accepted(PackedRoot16Entry entry) noexcept { return ((entry.value >> 36U) & 0x01U) != 0; }

constexpr bool byte_invalid(PackedByteEntry entry) noexcept { return ((entry.value >> 29U) & 0x01U) != 0; }

constexpr bool root16_invalid(PackedRoot16Entry entry) noexcept { return ((entry.value >> 37U) & 0x01U) != 0; }

constexpr std::uint8_t entry_out0(PackedByteEntry entry) noexcept {
    return static_cast<std::uint8_t>(entry.value & 0xffU);
}

constexpr std::uint8_t entry_out1(PackedByteEntry entry) noexcept {
    return static_cast<std::uint8_t>((entry.value >> 8U) & 0xffU);
}

constexpr std::uint8_t entry_out0(PackedRoot16Entry entry) noexcept {
    return static_cast<std::uint8_t>(entry.value & 0xffU);
}

constexpr std::uint8_t entry_out1(PackedRoot16Entry entry) noexcept {
    return static_cast<std::uint8_t>((entry.value >> 8U) & 0xffU);
}

constexpr std::uint8_t entry_out2(PackedRoot16Entry entry) noexcept {
    return static_cast<std::uint8_t>((entry.value >> 16U) & 0xffU);
}

constexpr DecodeTransition invalid_transition() noexcept {
    DecodeTransition transition{};
    transition.invalid = true;
    return transition;
}

constexpr void init_decode_build(DecodeBuildTables &build) noexcept {
    for (std::size_t i = 0; i < build.trie.size(); ++i) {
        build.trie[i] = make_trie_node();
        build.ending[i] = false;
        build.trie_to_state[i] = kInvalidState;
    }
    for (std::size_t i = 0; i < build.state_to_trie.size(); ++i) {
        build.state_to_trie[i] = kInvalidNode;
        build.queue[i] = kInvalidNode;
    }
}

constexpr void insert_code(DecodeBuildTables &build, std::uint16_t symbol, EncodeCode code) noexcept {
    std::uint16_t node = 0;
    for (std::uint8_t shift = code.bit_length; shift > 0; --shift) {
        const std::uint32_t bit = (code.bits >> (shift - 1U)) & 0x1U;
        std::uint16_t &child = bit == 0 ? build.trie[node].child0 : build.trie[node].child1;
        if (child == kInvalidNode) {
            child = build.trie_size++;
            build.trie[child] = make_trie_node();
        }
        node = child;
    }
    build.trie[node].symbol = symbol;
}

constexpr void mark_accepted_endings(DecodeBuildTables &build) noexcept {
    build.ending[0] = true;
    std::uint16_t node = 0;
    for (std::size_t depth = 0; depth < 7; ++depth) {
        node = build.trie[node].child1;
        build.ending[node] = true;
    }
}

constexpr void assign_decode_states(DecodeBuildTables &build) noexcept {
    build.trie_to_state[0] = 0;
    build.state_to_trie[0] = 0;
    build.queue[0] = 0;
    build.state_count = 1;

    std::size_t head = 0;
    std::size_t tail = 1;
    while (head < tail) {
        const std::uint16_t trie_node = build.queue[head++];
        const std::uint16_t children[2] = {build.trie[trie_node].child0, build.trie[trie_node].child1};
        for (std::uint16_t child: children) {
            if (child == kInvalidNode || build.trie[child].symbol != kNoSymbol ||
                build.trie_to_state[child] != kInvalidState) {
                continue;
            }
            if (build.state_count == build.state_to_trie.size()) {
                build.state_overflow = true;
                continue;
            }
            const std::uint16_t state = build.state_count++;
            build.trie_to_state[child] = state;
            build.state_to_trie[state] = child;
            build.queue[tail++] = child;
        }
    }
}

constexpr DecodeTransition advance_fixed_bits(const DecodeBuildTables &build, std::uint16_t start_trie_node,
                                              std::uint32_t bits, std::uint8_t bit_count,
                                              std::uint8_t max_output) noexcept {
    DecodeTransition transition{};
    std::uint16_t node = start_trie_node;

    for (std::uint8_t bit_index = 0; bit_index < bit_count; ++bit_index) {
        const std::uint8_t shift = static_cast<std::uint8_t>(bit_count - bit_index - 1U);
        const bool bit = ((bits >> shift) & 0x1U) != 0;
        node = bit ? build.trie[node].child1 : build.trie[node].child0;
        if (node == kInvalidNode) {
            return invalid_transition();
        }

        const std::uint16_t symbol = build.trie[node].symbol;
        if (symbol == kNoSymbol) {
            continue;
        }
        if (symbol == kEosSymbol) {
            return invalid_transition();
        }
        if (transition.emit_count == max_output || symbol > 0xffU) {
            transition.invalid = true;
            transition.output_overflow = true;
            return transition;
        }
        transition.out[transition.emit_count++] = static_cast<std::uint8_t>(symbol);
        node = 0;
    }

    transition.next_state = build.trie_to_state[node];
    if (transition.next_state == kInvalidState) {
        return invalid_transition();
    }
    transition.accepted = build.ending[node];
    return transition;
}

constexpr DecodeBuildTables build_decode_build_tables() noexcept {
    DecodeBuildTables build{};
    init_decode_build(build);
    for (std::size_t symbol = 0; symbol < kCodeTable.size(); ++symbol) {
        insert_code(build, static_cast<std::uint16_t>(symbol), kCodeTable[symbol]);
    }
    mark_accepted_endings(build);
    assign_decode_states(build);
    return build;
}

constexpr ByteDecodeRow build_byte_decode_row(const DecodeBuildTables &build, std::uint16_t state) noexcept {
    ByteDecodeRow row{};
    const PackedByteEntry invalid = pack_byte_entry(invalid_transition());

    if (state >= build.state_count) {
        for (std::uint16_t byte = 0; byte < 256U; ++byte) {
            row.entries[byte] = invalid;
        }
        return row;
    }

    const std::uint16_t trie_node = build.state_to_trie[state];
    for (std::uint16_t byte = 0; byte < 256U; ++byte) {
        const DecodeTransition transition = advance_fixed_bits(build, trie_node, byte, 8, 2);
        row.entries[byte] = pack_byte_entry(transition);
        row.output_overflow = row.output_overflow || transition.output_overflow;
    }

    return row;
}

constexpr void append_byte_output(DecodeTransition &transition, PackedByteEntry entry) noexcept {
    const std::uint8_t emit_count = byte_emit_count(entry);
    if (emit_count >= 1U) {
        transition.out[transition.emit_count++] = entry_out0(entry);
    }
    if (emit_count == 2U) {
        transition.out[transition.emit_count++] = entry_out1(entry);
    }
}

constexpr DecodeTransition compose_root16_entry(PackedByteEntry high, PackedByteEntry low) noexcept {
    if (byte_invalid(high) || byte_invalid(low)) {
        return invalid_transition();
    }

    DecodeTransition transition{};
    transition.emit_count = static_cast<std::uint8_t>(byte_emit_count(high) + byte_emit_count(low));
    if (transition.emit_count > 3U) {
        transition.invalid = true;
        transition.output_overflow = true;
        return transition;
    }

    transition.emit_count = 0;
    append_byte_output(transition, high);
    append_byte_output(transition, low);
    transition.next_state = byte_next_state(low);
    transition.accepted = byte_accepted(low);
    return transition;
}

constexpr Root16DecodeTable build_root16_decode_table(const ByteDecodeTables &byte_tables) noexcept {
    Root16DecodeTable root{};

    for (std::uint32_t idx = 0; idx < root.entries.size(); ++idx) {
        const PackedByteEntry high = byte_tables.rows[0]->entries[idx >> 8U];
        DecodeTransition transition{};
        if (byte_invalid(high)) {
            transition = invalid_transition();
        } else {
            const PackedByteEntry low = byte_tables.rows[byte_next_state(high)]->entries[idx & 0xffU];
            transition = compose_root16_entry(high, low);
        }
        root.entries[idx] = pack_root16_entry(transition);
        root.output_overflow = root.output_overflow || transition.output_overflow;
    }

    return root;
}

constexpr DecodeBuildTables kDecodeBuildTables = build_decode_build_tables();

template<std::size_t State>
constexpr ByteDecodeRow kByteDecodeRow = build_byte_decode_row(kDecodeBuildTables, static_cast<std::uint16_t>(State));

template<std::size_t... States>
constexpr std::array<const ByteDecodeRow *, sizeof...(States)>
build_byte_decode_row_refs(std::index_sequence<States...> /*unused*/) noexcept {
    return {&kByteDecodeRow<States>...};
}

template<std::size_t... States>
constexpr bool byte_decode_rows_output_overflow(std::index_sequence<States...> /*unused*/) noexcept {
    return (... || kByteDecodeRow<States>.output_overflow);
}

constexpr ByteDecodeTables build_byte_decode_tables() noexcept {
    return {.rows = build_byte_decode_row_refs(std::make_index_sequence<256>{}),
            .state_count = kDecodeBuildTables.state_count,
            .state_overflow = kDecodeBuildTables.state_overflow,
            .output_overflow = byte_decode_rows_output_overflow(std::make_index_sequence<256>{})};
}

constexpr ByteDecodeTables kByteDecodeTables = build_byte_decode_tables();
constexpr Root16DecodeTable kRoot16DecodeTable = build_root16_decode_table(kByteDecodeTables);

static_assert(!kByteDecodeTables.state_overflow, "HPACK Huffman decode table exceeded state capacity");
static_assert(!kByteDecodeTables.output_overflow, "HPACK Huffman byte table exceeded packed output capacity");
static_assert(!kRoot16DecodeTable.output_overflow, "HPACK Huffman root16 table exceeded packed output capacity");
static_assert(kByteDecodeTables.state_count == 256, "HPACK Huffman decode table must contain 256 states");
static_assert(root16_emit_count(kRoot16DecodeTable.entries[0]) == 3 &&
                      entry_out0(kRoot16DecodeTable.entries[0]) == '0' &&
                      entry_out1(kRoot16DecodeTable.entries[0]) == '0' &&
                      entry_out2(kRoot16DecodeTable.entries[0]) == '0',
              "HPACK Huffman root16 table must emit short-code symbols");

template<bool CheckDstCap>
HpackHuffmanEncodeResult hpack_huffman_encode_impl(const std::uint8_t *src, std::size_t len, std::uint8_t *dst,
                                                   std::size_t dst_cap, HpackHuffmanLowerMode lower_mode) noexcept {
    std::uint64_t bit_buffer = 0;
    std::uint8_t pending_bits = 0;
    std::size_t written = 0;
    std::size_t consumed = 0;
    const EncodeCode *code_table = select_encode_code_table(lower_mode);

    for (; consumed < len; ++consumed) {
        const EncodeCode code = code_table[src[consumed]];
        bit_buffer = (bit_buffer << code.bit_length) | code.bits;
        pending_bits = static_cast<std::uint8_t>(pending_bits + code.bit_length);

        while (pending_bits >= 8) {
            if constexpr (CheckDstCap) {
                if (written == dst_cap) {
                    return {.code = HpackHuffmanCode::OutputFull, .consumed = consumed, .written = written};
                }
            }
            pending_bits = static_cast<std::uint8_t>(pending_bits - 8);
            dst[written++] = static_cast<std::uint8_t>((bit_buffer >> pending_bits) & 0xffU);
        }
        bit_buffer = keep_low_bits(bit_buffer, pending_bits);
    }

    if (pending_bits != 0) {
        const std::uint8_t pad_bits = static_cast<std::uint8_t>(8 - pending_bits);
        bit_buffer = (bit_buffer << pad_bits) | ((std::uint64_t{1} << pad_bits) - 1U);
        pending_bits = static_cast<std::uint8_t>(pending_bits + pad_bits);
        if constexpr (CheckDstCap) {
            if (written == dst_cap) {
                return {.code = HpackHuffmanCode::OutputFull, .consumed = consumed, .written = written};
            }
        }
        dst[written++] = static_cast<std::uint8_t>(bit_buffer & 0xffU);
    }

    return {.code = HpackHuffmanCode::Ok, .consumed = consumed, .written = written};
}

template<bool WriteOutput, bool CheckDstCap>
HpackHuffmanDecodeResult hpack_huffman_decode_table_impl(HpackHuffmanDecodeState &state, const std::uint8_t *src,
                                                         std::size_t len, std::uint8_t *dst, std::size_t dst_cap,
                                                         bool last) noexcept {
    std::size_t written = 0;
    std::size_t consumed = 0;

    if (state.state >= kByteDecodeTables.state_count) {
        return {.code = HpackHuffmanCode::InvalidEncoding, .consumed = consumed, .written = written};
    }

    while (consumed < len) {
        if (state.state == 0 && consumed + 1U < len) {
            const bool has_root_output_space = !WriteOutput || !CheckDstCap || dst_cap - written >= 3U;
            if (has_root_output_space) {
                const auto idx = static_cast<std::uint16_t>((static_cast<std::uint16_t>(src[consumed]) << 8U) |
                                                            src[consumed + 1U]);
                const PackedRoot16Entry entry = kRoot16DecodeTable.entries[idx];
                if (root16_invalid(entry)) {
                    return {.code = HpackHuffmanCode::InvalidEncoding, .consumed = consumed, .written = written};
                }

                const std::uint8_t emit_count = root16_emit_count(entry);
                if constexpr (WriteOutput) {
                    if (emit_count >= 1U) {
                        dst[written] = entry_out0(entry);
                    }
                    if (emit_count >= 2U) {
                        dst[written + 1U] = entry_out1(entry);
                    }
                    if (emit_count == 3U) {
                        dst[written + 2U] = entry_out2(entry);
                    }
                }
                written += emit_count;
                consumed += 2U;
                state.state = root16_next_state(entry);
                state.ending = root16_accepted(entry);
                continue;
            }
        }

        const PackedByteEntry entry = kByteDecodeTables.rows[state.state]->entries[src[consumed]];
        if (byte_invalid(entry)) {
            return {.code = HpackHuffmanCode::InvalidEncoding, .consumed = consumed, .written = written};
        }
        const std::uint8_t emit_count = byte_emit_count(entry);

        if constexpr (WriteOutput && CheckDstCap) {
            if (written + emit_count > dst_cap) {
                return {.code = HpackHuffmanCode::OutputFull, .consumed = consumed, .written = written};
            }
        }

        if constexpr (WriteOutput) {
            if (emit_count >= 1U) {
                dst[written] = entry_out0(entry);
            }
            if (emit_count == 2U) {
                dst[written + 1U] = entry_out1(entry);
            }
        }
        written += emit_count;
        state.state = byte_next_state(entry);
        state.ending = byte_accepted(entry);
        ++consumed;
    }

    if (!last) {
        if (state.state != 0) {
            return {.code = HpackHuffmanCode::NeedMore, .consumed = consumed, .written = written};
        }
        return {.code = HpackHuffmanCode::Ok, .consumed = consumed, .written = written};
    }

    if (!state.ending) {
        return {.code = HpackHuffmanCode::InvalidEncoding, .consumed = consumed, .written = written};
    }

    state.reset();
    return {.code = HpackHuffmanCode::Ok, .consumed = consumed, .written = written};
}

} // namespace

std::size_t hpack_huffman_encoded_length(const std::uint8_t *src, std::size_t len,
                                         HpackHuffmanLowerMode lower_mode) noexcept {
    std::size_t bit_length = 0;
    const EncodeCode *code_table = select_encode_code_table(lower_mode);

    for (std::size_t i = 0; i < len; ++i) {
        bit_length += code_table[src[i]].bit_length;
    }

    return (bit_length + 7U) >> 3U;
}

std::size_t hpack_huffman_decoded_length(const std::uint8_t *src, std::size_t len, bool *ok) noexcept {
    HpackHuffmanDecodeState state;
    const HpackHuffmanDecodeResult result =
            hpack_huffman_decode_table_impl<false, false>(state, src, len, nullptr, 0, true);
    const bool valid = result.code == HpackHuffmanCode::Ok;

    if (ok) {
        *ok = valid;
    }
    return valid ? result.written : 0;
}

HpackHuffmanEncodeResult hpack_huffman_encode(const std::uint8_t *src, std::size_t len, std::uint8_t *dst,
                                              std::size_t dst_cap, HpackHuffmanLowerMode lower_mode) noexcept {
    return hpack_huffman_encode_impl<true>(src, len, dst, dst_cap, lower_mode);
}

HpackHuffmanEncodeResult hpack_huffman_encode_incremental(HpackHuffmanEncodeState &state, const std::uint8_t *src,
                                                          std::size_t len, std::uint8_t *dst, std::size_t dst_cap,
                                                          bool last, HpackHuffmanLowerMode lower_mode) noexcept {
    std::size_t consumed = 0;
    std::size_t written = 0;
    const EncodeCode *code_table = select_encode_code_table(lower_mode);

    auto flush_pending = [&]() -> bool {
        while (state.pending_bits >= 8) {
            if (written == dst_cap) {
                return false;
            }
            state.pending_bits = static_cast<std::uint8_t>(state.pending_bits - 8);
            dst[written++] = static_cast<std::uint8_t>((state.bit_buffer >> state.pending_bits) & 0xffU);
            state.bit_buffer = keep_low_bits(state.bit_buffer, state.pending_bits);
        }
        return true;
    };

    if (!state.finalizing) {
        while (consumed < len) {
            const EncodeCode code = code_table[src[consumed]];
            state.bit_buffer = (state.bit_buffer << code.bit_length) | code.bits;
            state.pending_bits = static_cast<std::uint8_t>(state.pending_bits + code.bit_length);
            ++consumed;

            if (!flush_pending()) {
                return {.code = HpackHuffmanCode::OutputFull, .consumed = consumed, .written = written};
            }
        }
    }

    if (!last && !state.finalizing) {
        return {.code = HpackHuffmanCode::Ok, .consumed = consumed, .written = written};
    }

    if (!state.finalizing) {
        if (state.pending_bits == 0) {
            state.reset();
            return {.code = HpackHuffmanCode::Ok, .consumed = consumed, .written = written};
        }
        const std::uint8_t pad_bits = static_cast<std::uint8_t>(8 - state.pending_bits);
        state.bit_buffer = (state.bit_buffer << pad_bits) | ((std::uint64_t{1} << pad_bits) - 1U);
        state.pending_bits = static_cast<std::uint8_t>(state.pending_bits + pad_bits);
        state.finalizing = true;
    }

    if (!flush_pending()) {
        return {.code = HpackHuffmanCode::OutputFull, .consumed = consumed, .written = written};
    }

    state.reset();
    return {.code = HpackHuffmanCode::Ok, .consumed = consumed, .written = written};
}

std::size_t hpack_huffman_encode_exact(const std::uint8_t *src, std::size_t len, std::uint8_t *dst,
                                       HpackHuffmanLowerMode lower_mode) noexcept {
    return hpack_huffman_encode_impl<false>(src, len, dst, 0, lower_mode).written;
}

HpackHuffmanDecodeResult hpack_huffman_decode(HpackHuffmanDecodeState &state, const std::uint8_t *src, std::size_t len,
                                              std::uint8_t *dst, std::size_t dst_cap, bool last) noexcept {
    return hpack_huffman_decode_table_impl<true, true>(state, src, len, dst, dst_cap, last);
}

HpackHuffmanDecodeResult hpack_huffman_decode_exact(HpackHuffmanDecodeState &state, const std::uint8_t *src,
                                                    std::size_t len, std::uint8_t *dst, bool last) noexcept {
    return hpack_huffman_decode_table_impl<true, false>(state, src, len, dst, 0, last);
}

} // namespace fiber::http
