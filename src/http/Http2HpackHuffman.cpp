#include "Http2HpackHuffman.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

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

struct DecodeCell {
    std::uint8_t next;
    std::uint8_t emit;
    std::uint8_t sym;
    std::uint8_t ending;
};

struct RawNibbleTransition {
    std::uint16_t next_trie_node;
    std::uint16_t symbol;
    bool valid;
};

struct DecodeTables {
    static constexpr std::uint16_t kInvalidNode = 0xffffU;
    static constexpr std::uint16_t kNoSymbol = 0xffffU;
    static constexpr std::uint16_t kEosSymbol = 256U;
    static constexpr std::size_t kMaxTrieNodes = 1024;
    static constexpr std::size_t kStateCount = 256;
    static constexpr std::uint16_t kInvalidState = 0xffffU;

    std::array<TrieNode, kMaxTrieNodes> trie{};
    std::array<bool, kMaxTrieNodes> ending{};
    std::array<std::array<DecodeCell, 16>, kStateCount> table{};
    std::uint16_t trie_size = 1;
    std::uint16_t state_count = 0;
    bool has_valid_self_loops = false;
};

constexpr std::array<EncodeCode, 257> kCodeTable{{
    {0x00001ff8, 13}, {0x007fffd8, 23}, {0x0fffffe2, 28}, {0x0fffffe3, 28},
    {0x0fffffe4, 28}, {0x0fffffe5, 28}, {0x0fffffe6, 28}, {0x0fffffe7, 28},
    {0x0fffffe8, 28}, {0x00ffffea, 24}, {0x3ffffffc, 30}, {0x0fffffe9, 28},
    {0x0fffffea, 28}, {0x3ffffffd, 30}, {0x0fffffeb, 28}, {0x0fffffec, 28},
    {0x0fffffed, 28}, {0x0fffffee, 28}, {0x0fffffef, 28}, {0x0ffffff0, 28},
    {0x0ffffff1, 28}, {0x0ffffff2, 28}, {0x3ffffffe, 30}, {0x0ffffff3, 28},
    {0x0ffffff4, 28}, {0x0ffffff5, 28}, {0x0ffffff6, 28}, {0x0ffffff7, 28},
    {0x0ffffff8, 28}, {0x0ffffff9, 28}, {0x0ffffffa, 28}, {0x0ffffffb, 28},
    {0x00000014,  6}, {0x000003f8, 10}, {0x000003f9, 10}, {0x00000ffa, 12},
    {0x00001ff9, 13}, {0x00000015,  6}, {0x000000f8,  8}, {0x000007fa, 11},
    {0x000003fa, 10}, {0x000003fb, 10}, {0x000000f9,  8}, {0x000007fb, 11},
    {0x000000fa,  8}, {0x00000016,  6}, {0x00000017,  6}, {0x00000018,  6},
    {0x00000000,  5}, {0x00000001,  5}, {0x00000002,  5}, {0x00000019,  6},
    {0x0000001a,  6}, {0x0000001b,  6}, {0x0000001c,  6}, {0x0000001d,  6},
    {0x0000001e,  6}, {0x0000001f,  6}, {0x0000005c,  7}, {0x000000fb,  8},
    {0x00007ffc, 15}, {0x00000020,  6}, {0x00000ffb, 12}, {0x000003fc, 10},
    {0x00001ffa, 13}, {0x00000021,  6}, {0x0000005d,  7}, {0x0000005e,  7},
    {0x0000005f,  7}, {0x00000060,  7}, {0x00000061,  7}, {0x00000062,  7},
    {0x00000063,  7}, {0x00000064,  7}, {0x00000065,  7}, {0x00000066,  7},
    {0x00000067,  7}, {0x00000068,  7}, {0x00000069,  7}, {0x0000006a,  7},
    {0x0000006b,  7}, {0x0000006c,  7}, {0x0000006d,  7}, {0x0000006e,  7},
    {0x0000006f,  7}, {0x00000070,  7}, {0x00000071,  7}, {0x00000072,  7},
    {0x000000fc,  8}, {0x00000073,  7}, {0x000000fd,  8}, {0x00001ffb, 13},
    {0x0007fff0, 19}, {0x00001ffc, 13}, {0x00003ffc, 14}, {0x00000022,  6},
    {0x00007ffd, 15}, {0x00000003,  5}, {0x00000023,  6}, {0x00000004,  5},
    {0x00000024,  6}, {0x00000005,  5}, {0x00000025,  6}, {0x00000026,  6},
    {0x00000027,  6}, {0x00000006,  5}, {0x00000074,  7}, {0x00000075,  7},
    {0x00000028,  6}, {0x00000029,  6}, {0x0000002a,  6}, {0x00000007,  5},
    {0x0000002b,  6}, {0x00000076,  7}, {0x0000002c,  6}, {0x00000008,  5},
    {0x00000009,  5}, {0x0000002d,  6}, {0x00000077,  7}, {0x00000078,  7},
    {0x00000079,  7}, {0x0000007a,  7}, {0x0000007b,  7}, {0x00007ffe, 15},
    {0x000007fc, 11}, {0x00003ffd, 14}, {0x00001ffd, 13}, {0x0ffffffc, 28},
    {0x000fffe6, 20}, {0x003fffd2, 22}, {0x000fffe7, 20}, {0x000fffe8, 20},
    {0x003fffd3, 22}, {0x003fffd4, 22}, {0x003fffd5, 22}, {0x007fffd9, 23},
    {0x003fffd6, 22}, {0x007fffda, 23}, {0x007fffdb, 23}, {0x007fffdc, 23},
    {0x007fffdd, 23}, {0x007fffde, 23}, {0x00ffffeb, 24}, {0x007fffdf, 23},
    {0x00ffffec, 24}, {0x00ffffed, 24}, {0x003fffd7, 22}, {0x007fffe0, 23},
    {0x00ffffee, 24}, {0x007fffe1, 23}, {0x007fffe2, 23}, {0x007fffe3, 23},
    {0x007fffe4, 23}, {0x001fffdc, 21}, {0x003fffd8, 22}, {0x007fffe5, 23},
    {0x003fffd9, 22}, {0x007fffe6, 23}, {0x007fffe7, 23}, {0x00ffffef, 24},
    {0x003fffda, 22}, {0x001fffdd, 21}, {0x000fffe9, 20}, {0x003fffdb, 22},
    {0x003fffdc, 22}, {0x007fffe8, 23}, {0x007fffe9, 23}, {0x001fffde, 21},
    {0x007fffea, 23}, {0x003fffdd, 22}, {0x003fffde, 22}, {0x00fffff0, 24},
    {0x001fffdf, 21}, {0x003fffdf, 22}, {0x007fffeb, 23}, {0x007fffec, 23},
    {0x001fffe0, 21}, {0x001fffe1, 21}, {0x003fffe0, 22}, {0x001fffe2, 21},
    {0x007fffed, 23}, {0x003fffe1, 22}, {0x007fffee, 23}, {0x007fffef, 23},
    {0x000fffea, 20}, {0x003fffe2, 22}, {0x003fffe3, 22}, {0x003fffe4, 22},
    {0x007ffff0, 23}, {0x003fffe5, 22}, {0x003fffe6, 22}, {0x007ffff1, 23},
    {0x03ffffe0, 26}, {0x03ffffe1, 26}, {0x000fffeb, 20}, {0x0007fff1, 19},
    {0x003fffe7, 22}, {0x007ffff2, 23}, {0x003fffe8, 22}, {0x01ffffec, 25},
    {0x03ffffe2, 26}, {0x03ffffe3, 26}, {0x03ffffe4, 26}, {0x07ffffde, 27},
    {0x07ffffdf, 27}, {0x03ffffe5, 26}, {0x00fffff1, 24}, {0x01ffffed, 25},
    {0x0007fff2, 19}, {0x001fffe3, 21}, {0x03ffffe6, 26}, {0x07ffffe0, 27},
    {0x07ffffe1, 27}, {0x03ffffe7, 26}, {0x07ffffe2, 27}, {0x00fffff2, 24},
    {0x001fffe4, 21}, {0x001fffe5, 21}, {0x03ffffe8, 26}, {0x03ffffe9, 26},
    {0x0ffffffd, 28}, {0x07ffffe3, 27}, {0x07ffffe4, 27}, {0x07ffffe5, 27},
    {0x000fffec, 20}, {0x00fffff3, 24}, {0x000fffed, 20}, {0x001fffe6, 21},
    {0x003fffe9, 22}, {0x001fffe7, 21}, {0x001fffe8, 21}, {0x007ffff3, 23},
    {0x003fffea, 22}, {0x003fffeb, 22}, {0x01ffffee, 25}, {0x01ffffef, 25},
    {0x00fffff4, 24}, {0x00fffff5, 24}, {0x03ffffea, 26}, {0x007ffff4, 23},
    {0x03ffffeb, 26}, {0x07ffffe6, 27}, {0x03ffffec, 26}, {0x03ffffed, 26},
    {0x07ffffe7, 27}, {0x07ffffe8, 27}, {0x07ffffe9, 27}, {0x07ffffea, 27},
    {0x07ffffeb, 27}, {0x0ffffffe, 28}, {0x07ffffec, 27}, {0x07ffffed, 27},
    {0x07ffffee, 27}, {0x07ffffef, 27}, {0x07fffff0, 27}, {0x03ffffee, 26},
    {0x3fffffff, 30},
}};

constexpr TrieNode make_trie_node() noexcept {
    return {DecodeTables::kInvalidNode, DecodeTables::kInvalidNode, DecodeTables::kNoSymbol};
}

constexpr std::uint8_t to_lower_ascii(std::uint8_t ch) noexcept {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<std::uint8_t>(ch + ('a' - 'A'));
    }
    return ch;
}

constexpr RawNibbleTransition advance_nibble(const DecodeTables &tables, std::uint16_t trie_node,
                                             std::uint8_t nibble) noexcept {
    std::uint16_t node = trie_node;
    std::uint16_t emitted = DecodeTables::kNoSymbol;

    for (int shift = 3; shift >= 0; --shift) {
        const bool bit = ((nibble >> shift) & 0x1U) != 0;
        node = bit ? tables.trie[node].child1 : tables.trie[node].child0;
        if (node == DecodeTables::kInvalidNode) {
            return {0, 0, false};
        }

        const std::uint16_t symbol = tables.trie[node].symbol;
        if (symbol == DecodeTables::kNoSymbol) {
            continue;
        }

        if (symbol == DecodeTables::kEosSymbol || emitted != DecodeTables::kNoSymbol) {
            return {0, 0, false};
        }

        emitted = symbol;
        node = 0;
    }

    return {node, emitted, true};
}

constexpr DecodeTables build_decode_tables() noexcept {
    DecodeTables tables{};

    for (std::size_t i = 0; i < tables.trie.size(); ++i) {
        tables.trie[i] = make_trie_node();
        tables.ending[i] = false;
    }

    for (std::uint16_t symbol = 0; symbol < kCodeTable.size(); ++symbol) {
        std::uint16_t node = 0;
        const EncodeCode code = kCodeTable[symbol];

        for (std::uint8_t shift = code.bit_length; shift > 0; --shift) {
            const std::uint32_t bit = (code.bits >> (shift - 1U)) & 0x1U;
            std::uint16_t &child = bit == 0 ? tables.trie[node].child0 : tables.trie[node].child1;
            if (child == DecodeTables::kInvalidNode) {
                child = tables.trie_size++;
                tables.trie[child] = make_trie_node();
            }
            node = child;
        }

        tables.trie[node].symbol = symbol;
    }

    tables.ending[0] = true;
    std::uint16_t node = 0;
    for (std::size_t depth = 0; depth < 7; ++depth) {
        node = tables.trie[node].child1;
        tables.ending[node] = true;
    }

    std::array<std::uint16_t, DecodeTables::kMaxTrieNodes> trie_to_state{};
    std::array<std::uint16_t, DecodeTables::kStateCount> state_to_trie{};
    std::array<std::uint16_t, DecodeTables::kStateCount> queue{};

    for (std::size_t i = 0; i < trie_to_state.size(); ++i) {
        trie_to_state[i] = DecodeTables::kInvalidState;
    }

    trie_to_state[0] = 0;
    state_to_trie[0] = 0;
    queue[0] = 0;
    tables.state_count = 1;

    std::size_t head = 0;
    std::size_t tail = 1;

    while (head < tail) {
        const std::uint16_t trie_state = queue[head++];
        const std::uint16_t state_id = trie_to_state[trie_state];

        for (std::uint8_t nibble = 0; nibble < 16; ++nibble) {
            const RawNibbleTransition step = advance_nibble(tables, trie_state, nibble);
            if (!step.valid) {
                tables.table[state_id][nibble] = {
                    static_cast<std::uint8_t>(state_id), 0, 0, 0,
                };
                continue;
            }

            std::uint16_t next_state = trie_to_state[step.next_trie_node];
            if (next_state == DecodeTables::kInvalidState) {
                next_state = tables.state_count++;
                trie_to_state[step.next_trie_node] = next_state;
                state_to_trie[next_state] = step.next_trie_node;
                queue[tail++] = step.next_trie_node;
            }

            if (next_state == state_id) {
                tables.has_valid_self_loops = true;
            }

            tables.table[state_id][nibble] = {
                static_cast<std::uint8_t>(next_state),
                static_cast<std::uint8_t>(step.symbol != DecodeTables::kNoSymbol),
                static_cast<std::uint8_t>(step.symbol == DecodeTables::kNoSymbol ? 0 : step.symbol),
                static_cast<std::uint8_t>(tables.ending[step.next_trie_node]),
            };
        }
    }

    return tables;
}

constexpr DecodeTables kDecodeTables = build_decode_tables();

static_assert(kDecodeTables.state_count == DecodeTables::kStateCount,
              "HPACK Huffman decode DFA must contain 256 states");
static_assert(!kDecodeTables.has_valid_self_loops,
              "HPACK Huffman decode DFA uses self-loops as invalid transition sentinels");

} // namespace

std::size_t http2_huffman_encoded_length(const std::uint8_t *src, std::size_t len,
                                         Http2HuffmanLowerMode lower_mode) noexcept {
    std::size_t bit_length = 0;

    for (std::size_t i = 0; i < len; ++i) {
        std::uint8_t ch = src[i];
        if (lower_mode == Http2HuffmanLowerMode::Ascii) {
            ch = to_lower_ascii(ch);
        }
        bit_length += kCodeTable[ch].bit_length;
    }

    return (bit_length + 7U) >> 3U;
}

std::size_t http2_huffman_decoded_length(const std::uint8_t *src, std::size_t len, bool *ok) noexcept {
    Http2HuffmanDecodeState state;
    std::array<std::uint8_t, 64> scratch{};
    std::size_t total = 0;
    std::size_t offset = 0;
    bool valid = true;

    while (offset < len) {
        Http2HuffmanDecodeResult result =
            http2_huffman_decode(state, src + offset, len - offset, scratch.data(), scratch.size(), true);
        total += result.written;
        offset += result.consumed;
        if (result.code == Http2HuffmanCode::Ok) {
            break;
        }
        if (result.code == Http2HuffmanCode::OutputFull) {
            continue;
        }
        valid = false;
        break;
    }

    if (ok) {
        *ok = valid;
    }
    return valid ? total : 0;
}

Http2HuffmanEncodeResult http2_huffman_encode(const std::uint8_t *src, std::size_t len, std::uint8_t *dst,
                                              std::size_t dst_cap, Http2HuffmanLowerMode lower_mode) noexcept {
    std::uint64_t bit_buffer = 0;
    std::uint8_t pending_bits = 0;
    std::size_t written = 0;

    for (std::size_t i = 0; i < len; ++i) {
        std::uint8_t ch = src[i];
        if (lower_mode == Http2HuffmanLowerMode::Ascii) {
            ch = to_lower_ascii(ch);
        }

        const EncodeCode code = kCodeTable[ch];
        bit_buffer = (bit_buffer << code.bit_length) | code.bits;
        pending_bits = static_cast<std::uint8_t>(pending_bits + code.bit_length);

        while (pending_bits >= 8) {
            if (written == dst_cap) {
                return {Http2HuffmanCode::OutputFull, written};
            }
            pending_bits = static_cast<std::uint8_t>(pending_bits - 8);
            dst[written++] = static_cast<std::uint8_t>((bit_buffer >> pending_bits) & 0xffU);
        }
    }

    if (pending_bits != 0) {
        const std::uint8_t pad_bits = static_cast<std::uint8_t>(8 - pending_bits);
        bit_buffer = (bit_buffer << pad_bits) | ((std::uint64_t{1} << pad_bits) - 1U);
        pending_bits = static_cast<std::uint8_t>(pending_bits + pad_bits);
        if (written == dst_cap) {
            return {Http2HuffmanCode::OutputFull, written};
        }
        dst[written++] = static_cast<std::uint8_t>(bit_buffer & 0xffU);
    }

    return {Http2HuffmanCode::Ok, written};
}

Http2HuffmanDecodeResult http2_huffman_decode(Http2HuffmanDecodeState &state, const std::uint8_t *src, std::size_t len,
                                              std::uint8_t *dst, std::size_t dst_cap, bool last) noexcept {
    std::size_t written = 0;
    std::size_t consumed = 0;

    for (std::size_t i = 0; i < len; ++i) {
        std::uint8_t next_state = state.state;
        std::uint8_t emitted[2]{};
        std::size_t emit_count = 0;
        const std::uint8_t ch = src[i];
        bool ending = state.ending;

        const DecodeCell hi = kDecodeTables.table[next_state][ch >> 4];
        if (hi.next == next_state) {
            return {Http2HuffmanCode::InvalidEncoding, consumed, written};
        }
        next_state = hi.next;
        ending = hi.ending != 0;
        if (hi.emit != 0) {
            emitted[emit_count++] = hi.sym;
        }

        const DecodeCell lo = kDecodeTables.table[next_state][ch & 0x0fU];
        if (lo.next == next_state) {
            return {Http2HuffmanCode::InvalidEncoding, consumed, written};
        }
        next_state = lo.next;
        ending = lo.ending != 0;
        if (lo.emit != 0) {
            emitted[emit_count++] = lo.sym;
        }

        if (written + emit_count > dst_cap) {
            return {Http2HuffmanCode::OutputFull, consumed, written};
        }

        if (emit_count != 0) {
            std::memcpy(dst + written, emitted, emit_count);
            written += emit_count;
        }

        state.state = next_state;
        state.ending = ending;
        ++consumed;
    }

    if (!last) {
        if (state.state != 0) {
            return {Http2HuffmanCode::NeedMore, consumed, written};
        }
        return {Http2HuffmanCode::Ok, consumed, written};
    }

    if (!state.ending) {
        return {Http2HuffmanCode::InvalidEncoding, consumed, written};
    }

    state.reset();
    return {Http2HuffmanCode::Ok, consumed, written};
}

} // namespace fiber::http
