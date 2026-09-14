#ifndef FIBER_COMPRESSION_DEFLATE_STATE_H
#define FIBER_COMPRESSION_DEFLATE_STATE_H

// Private raw-DEFLATE encoder state, derived from zlib 1.3.2 deflate.h /
// deflate.c (Jean-loup Gailly; see src/compression/UPSTREAM.md). All stream
// parameters are fixed to the upstream defaults used by the HTTP gzip path:
// windowBits 15, memLevel 8, default strategy, levels 1..9. Gzip framing,
// CRC and ISIZE are handled by the caller (GzipEncoder); this state emits a
// bare DEFLATE stream.
//
// The state is not self-contained: DeflateEncoder::init() binds the window,
// hash heads/tails and pending buffer out of a caller-provided workspace
// (single allocation). A bound state must not be moved or copied.

#include <cstddef>
#include <cstdint>

#include "DeflateTables.h"

namespace fiber::compression {

// ---- fixed stream parameters ----
inline constexpr std::uint32_t kDeflateWBits = 15;
inline constexpr std::uint32_t kDeflateWSize = 1u << kDeflateWBits; // 32768
inline constexpr std::uint32_t kDeflateWMask = kDeflateWSize - 1;
inline constexpr std::uint64_t kDeflateWindowSize = 2ull * kDeflateWSize; // 65536
inline constexpr std::uint32_t kDeflateMemLevel = 8;
inline constexpr std::uint32_t kDeflateHashBits = kDeflateMemLevel + 7; // 15
inline constexpr std::uint32_t kDeflateHashSize = 1u << kDeflateHashBits;
inline constexpr std::uint32_t kDeflateHashMask = kDeflateHashSize - 1;
inline constexpr std::uint32_t kDeflateHashShift = (kDeflateHashBits + kDeflateMinMatch - 1) / kDeflateMinMatch; // 5
inline constexpr std::uint32_t kDeflateLitBufSize = 1u << (kDeflateMemLevel + 6); // 16384
inline constexpr std::uint32_t kDeflatePendingBufSize = kDeflateLitBufSize * 4; // 65536
// Symbols are accumulated into the tail of the pending buffer (three bytes
// each); the block is flushed before the two regions overlap.
inline constexpr std::uint32_t kDeflateSymEnd = (kDeflateLitBufSize - 1) * 3; // 49149

// ---- window invariants ----
inline constexpr std::uint32_t kDeflateMinLookahead = kDeflateMaxMatch + kDeflateMinMatch + 1; // 262
inline constexpr std::uint32_t kDeflateMaxDist = kDeflateWSize - kDeflateMinLookahead; // 32506
inline constexpr std::uint32_t kDeflateWinInit = kDeflateMaxMatch; // 258
inline constexpr std::uint32_t kDeflateTooFar = 4096;

inline constexpr std::uint16_t kDeflateNil = 0; // hash chain terminator (zlib NIL for Pos)

using DeflatePos = std::uint16_t; // window position that fits in w_size
using DeflateIPos = std::uint32_t; // window position before masking (unsigned IPos)

// Tree descriptor: dynamic tree plus a pointer to the static descriptor
// (zlib tree_desc). The dynamic trees live inside DeflateState; the static
// data lives in DeflateTables.h.
struct DeflateTreeDesc {
    DeflateCtData *dyn_tree = nullptr; // the dynamic tree
    int max_code = 0; // largest code with non-zero frequency
    const DeflateStaticTreeDesc *stat_desc = nullptr;
};

enum class DeflateStreamStatus : std::uint8_t {
    Busy, // data accepted, stream open
    FinishStarted, // final block in progress (zlib FINISH_STATE)
};

// Mirrors zlib's internal_state with the removed features (gzip header
// handling, stored/RLE/HUFF strategies, dictionary, tune/copy, diagnostics)
// dropped. Widths are tightened to provable ranges: block_start >=
// strstart - 32768 >= -32768 and high_water <= window_size, so both fit in
// 32 bits.
struct DeflateState {
    // ---- pending output ----
    std::uint8_t *pending_buf = nullptr; // output buffer (overlaid with symbols at +lit_bufsize)
    std::uint8_t *pending_out = nullptr; // next pending byte to emit
    std::uint32_t pending = 0; // bytes pending emission
    DeflateStreamStatus status = DeflateStreamStatus::Busy;
    int last_flush = -2; // value of last flush op: -2..4 (None/Sync/Finish)
    // Port addition (no upstream counterpart): upstream reports a completed
    // sync flush implicitly, by returning with avail_out > 0. When a caller's
    // output buffer fills exactly, that signal is invisible and the next call
    // would emit a second marker. flush() tracks the request explicitly:
    // `flush_marker_owed` until the empty stored block is emitted, and
    // Flushed is reported only when the marker is out and pending == 0.
    bool flush_marker_owed = false; // sync flush requested, marker not yet emitted
    bool sync_flush_active = false; // a flush() request is mid-flight

    // ---- sliding window and hash chains ----
    std::uint8_t *window = nullptr; // 2*w_size bytes with a 258-byte sentinel gap
    DeflatePos *prev = nullptr; // hash chain links (w_size entries)
    DeflatePos *head = nullptr; // heads of hash chains (hash_size entries)
    std::uint32_t ins_h = 0; // hash index of string to be inserted
    std::int32_t block_start = 0; // window position of the start of the current block
    std::uint32_t strstart = 0; // window position of the current string
    std::uint32_t match_start = 0; // window position of the current match
    std::uint32_t lookahead = 0; // number of valid bytes ahead of strstart
    std::uint32_t match_length = 0; // length of the best match at the previous step
    DeflateIPos prev_match = 0; // match at the previous step
    std::uint32_t prev_length = 0; // length of the best match at the previous step
    bool match_available = false; // set if a previous match exists (lazy evaluation)
    std::uint32_t max_chain_length = 0;
    std::uint32_t max_lazy_match = 0;
    std::uint32_t good_match = 0;
    std::int32_t nice_match = 0; // stop searching when a match >= this is found
    int level = 1; // 1..9

    // ---- Huffman trees ----
    DeflateCtData dyn_ltree[kDeflateHeapSize]; // literal and length codes
    DeflateCtData dyn_dtree[2 * kDeflateDCodes + 1]; // distance codes
    DeflateCtData bl_tree[2 * kDeflateBCodes + 1]; // Huffman tree for bit lengths
    DeflateTreeDesc l_desc;
    DeflateTreeDesc d_desc;
    DeflateTreeDesc bl_desc;
    std::uint16_t bl_count[kDeflateMaxBits + 1];
    int heap[kDeflateHeapSize]; // heap used to build trees
    int heap_len = 0; // number of elements in the heap
    int heap_max = 0; // element of the largest frequency
    std::uint8_t depth[kDeflateHeapSize]; // depth of each subtree (max_length+1 bits)

    // ---- symbol buffer ----
    // sym_buf == pending_buf + lit_bufsize; three bytes per symbol:
    // distance low, distance high, length/literal.
    std::uint32_t sym_next = 0; // index of next free slot (byte offset)

    // ---- block cost accounting ----
    std::uint64_t opt_len = 0; // bit length of current block with optimal trees
    std::uint64_t static_len = 0; // bit length of current block with static trees

    // ---- insertion backlog ----
    std::uint32_t insert = 0; // bytes at index strstart-inset..strstart-1 to insert

    // ---- bit accumulator ----
    std::uint16_t bi_buf = 0;
    int bi_valid = 0; // number of valid bits in bi_buf

    // ---- window bookkeeping ----
    std::uint32_t high_water = 0; // highest window position written+zeroed
    bool slid = false; // whether the window was slid once

    [[nodiscard]] const std::uint8_t *sym_buf() const noexcept { return pending_buf + kDeflateLitBufSize; }
    [[nodiscard]] std::uint8_t *sym_buf() noexcept { return pending_buf + kDeflateLitBufSize; }

    [[nodiscard]] bool finish_started() const noexcept { return status == DeflateStreamStatus::FinishStarted; }
};

// Rank of a flush op, for duplicate-op suppression (zlib RANK: 2*f with the
// f > 4 adjustment dropped because Finish is the largest op we support).
inline constexpr int deflate_rank(int flush) noexcept { return 2 * flush; }

// One of the 30 distance codes (zlib d_code).
inline constexpr unsigned deflate_d_code(std::uint32_t dist) noexcept {
    return dist < 256 ? kDeflateDistCode[dist] : kDeflateDistCode[256 + (dist >> 7)];
}

// Symbol tally fast paths (zlib _tr_tally_lit / _tr_tally_dist). `flush` is
// set when the symbol buffer is full and the block must be emitted.
inline void deflate_tally_lit(DeflateState &s, std::uint8_t cc, bool &flush) noexcept {
    std::uint8_t *sym_next = s.sym_buf() + s.sym_next;
    s.sym_next += 3;
    *sym_next++ = 0;
    *sym_next++ = 0;
    *sym_next++ = cc;
    s.dyn_ltree[cc].freq++;
    flush = s.sym_next == kDeflateSymEnd;
}

inline void deflate_tally_dist(DeflateState &s, std::uint32_t distance, std::uint8_t lc, bool &flush) noexcept {
    std::uint8_t *sym_next = s.sym_buf() + s.sym_next;
    s.sym_next += 3;
    *sym_next++ = static_cast<std::uint8_t>(distance);
    *sym_next++ = static_cast<std::uint8_t>(distance >> 8);
    *sym_next++ = lc;
    --distance;
    s.dyn_ltree[kDeflateLengthCode[lc] + kDeflateLiterals + 1].freq++;
    s.dyn_dtree[deflate_d_code(distance)].freq++;
    flush = s.sym_next == kDeflateSymEnd;
}

// ---- trees.c interface (DeflateTrees.cpp) ----

// Initialize the tree descriptors and start a new empty block.
void deflate_tr_init(DeflateState &s) noexcept;

// Flush the bit accumulator into the pending buffer.
void deflate_tr_flush_bits(DeflateState &s) noexcept;

// Align the output on a byte boundary without a stored block (used by partial
// flushes).
void deflate_tr_align(DeflateState &s) noexcept;

// Emit a stored block (possibly empty; buf may be null when stored_len is 0).
void deflate_tr_stored_block(DeflateState &s, const std::uint8_t *buf, std::uint64_t stored_len, int last) noexcept;

// Choose the cheapest encoding for the accumulated symbols and emit them as a
// (possibly last) block.
void deflate_tr_flush_block(DeflateState &s, const std::uint8_t *buf, std::uint64_t stored_len, int last) noexcept;

} // namespace fiber::compression

#endif // FIBER_COMPRESSION_DEFLATE_STATE_H
