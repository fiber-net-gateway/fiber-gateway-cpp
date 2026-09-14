// Private resumable DEFLATE encoder, ported from zlib 1.3.2 deflate.c
// (Jean-loup Gailly; see src/compression/UPSTREAM.md). Removed upstream
// features: zlib/gzip wrappers (this layer emits a raw stream; GzipEncoder
// adds framing), the stored-only (level 0), RLE and Huffman-only strategies,
// preset dictionaries, mid-stream parameter changes, deflateCopy/Prime/
// Pending/Bound, and the debug counters. The zlib deflate() driver loop is
// restructured into resumable write/flush/finish operations, but the control
// flow — pending-first draining, RANK-based duplicate-flush suppression, the
// last_flush == -1 sentinel after a partial drain, and the FINISH_STATE re-
// entry guard — follows upstream exactly so the byte stream is identical.

#include "DeflateEncoder.h"

#include <cstring>

#include <fiber/common/Assert.h>

namespace fiber::compression {

namespace {

// Flush op encodings (zlib values: 0/2/4; the rank of an op is 2*value).
inline constexpr int kFlushNone = 0;
inline constexpr int kFlushSync = 2;
inline constexpr int kFlushFinish = 4;

// Per-level tuning (zlib configuration_table rows 1..9; row 0 is the stored
// driver, dropped). good/chain are unused by the fast levels' match search
// shortcuts but kept for fidelity.
struct DeflateConfig {
    std::uint32_t good_length;
    std::uint32_t max_lazy;
    std::uint32_t nice_length;
    std::uint32_t max_chain;
};

inline constexpr DeflateConfig kConfigTable[10] = {
        {0, 0, 0, 0}, // level 0 (stored driver, unused)
        {4, 4, 8, 4}, // 1: max speed, no lazy matches
        {4, 5, 16, 8}, // 2
        {4, 6, 32, 32}, // 3
        {4, 4, 16, 16}, // 4: lazy matches
        {8, 16, 32, 32}, // 5
        {8, 16, 128, 128}, // 6: zlib default
        {8, 32, 128, 256}, // 7
        {32, 128, 258, 1024}, // 8
        {32, 258, 258, 4096}, // 9: max compression
};

// Input/output cursors standing in for z_stream fields. `total` accumulates
// the bytes handed to the caller across all flush_pending calls of one
// operation.
struct DeflateIn {
    const std::uint8_t *next = nullptr;
    std::size_t avail = 0;
};

struct DeflateOut {
    std::uint8_t *next = nullptr;
    std::size_t avail = 0;
    std::size_t total = 0;
};

enum class BlockState { NeedMore, BlockDone, FinishStarted, FinishDone };

enum class RunResult { Ok, NothingToDo, StreamEnd };

inline void update_hash(DeflateState &s, std::uint32_t &h, std::uint8_t c) noexcept {
    h = ((h << kDeflateHashShift) ^ c) & kDeflateHashMask;
}

// Insert the string at window position `str` into the hash chains and return
// the previous chain head (kDeflateNil when the chain was empty).
inline DeflateIPos insert_string(DeflateState &s, std::uint32_t str) noexcept {
    update_hash(s, s.ins_h, s.window[str + (kDeflateMinMatch - 1)]);
    const DeflateIPos match_head = s.head[s.ins_h];
    s.prev[str & kDeflateWMask] = s.head[s.ins_h];
    s.head[s.ins_h] = static_cast<DeflatePos>(str);
    return match_head;
}

inline void clear_hash(DeflateState &s) noexcept {
    s.head[kDeflateHashSize - 1] = kDeflateNil;
    std::memset(s.head, 0, static_cast<std::size_t>(kDeflateHashSize - 1) * sizeof(DeflatePos));
    s.slid = false;
}

void slide_hash(DeflateState &s) noexcept {
    const std::uint32_t wsize = kDeflateWSize;

    std::uint32_t n = kDeflateHashSize;
    DeflatePos *p = &s.head[n];
    do {
        const std::uint16_t m = *--p;
        *p = static_cast<DeflatePos>(m >= wsize ? m - wsize : kDeflateNil);
    } while (--n != 0);

    n = kDeflateWSize;
    p = &s.prev[n];
    do {
        const std::uint16_t m = *--p;
        *p = static_cast<DeflatePos>(m >= wsize ? m - wsize : kDeflateNil);
        // If n is not on any hash chain, prev[n] is garbage but its value
        // will never be used.
    } while (--n != 0);
    s.slid = true;
}

// Copy from the caller's input into `buf`, at most `size` bytes.
unsigned read_buf(DeflateState & /*s*/, DeflateIn &in, std::uint8_t *buf, unsigned size) noexcept {
    unsigned len = static_cast<unsigned>(in.avail);
    if (len > size) {
        len = size;
    }
    if (len == 0) {
        return 0;
    }

    in.avail -= len;
    std::memcpy(buf, in.next, len);
    in.next += len;
    return len;
}

// Flush the bit accumulator, then move pending bytes to the caller's output.
void flush_pending(DeflateState &s, DeflateOut &out) noexcept {
    deflate_tr_flush_bits(s);
    unsigned len = s.pending > out.avail ? static_cast<unsigned>(out.avail) : s.pending;
    if (len == 0) {
        return;
    }

    std::memcpy(out.next, s.pending_out, len);
    out.next += len;
    out.total += len;
    out.avail -= len;
    s.pending_out += len;
    s.pending -= len;
    if (s.pending == 0) {
        s.pending_out = s.pending_buf;
    }
}

// Flush the current block, with given end-of-file flag (zlib FLUSH_BLOCK_ONLY).
#define FIBER_DEFLATE_FLUSH_BLOCK_ONLY(s, out, last)                                                                   \
    do {                                                                                                               \
        deflate_tr_flush_block(                                                                                        \
                *(s), (s)->block_start >= 0 ? (s)->window + static_cast<unsigned>((s)->block_start) : nullptr,         \
                static_cast<std::uint64_t>(static_cast<std::int64_t>((s)->strstart) - (s)->block_start),               \
                (last) ? 1 : 0);                                                                                       \
        (s)->block_start = static_cast<std::int32_t>((s)->strstart);                                                   \
        flush_pending(*(s), (out));                                                                                    \
    } while (0)

// Same but force premature exit if necessary (zlib FLUSH_BLOCK).
#define FIBER_DEFLATE_FLUSH_BLOCK(s, out, last)                                                                        \
    do {                                                                                                               \
        FIBER_DEFLATE_FLUSH_BLOCK_ONLY((s), (out), (last));                                                            \
        if ((out).avail == 0) {                                                                                        \
            return (last) ? BlockState::FinishStarted : BlockState::NeedMore;                                          \
        }                                                                                                              \
    } while (0)

// Fill the window when the lookahead becomes insufficient. Updates strstart
// and lookahead. Invariant on entry: lookahead < MIN_LOOKAHEAD.
void fill_window(DeflateState &s, DeflateIn &in) noexcept {
    unsigned n;
    unsigned more; // amount of free space at the end of the window

    do {
        more = static_cast<unsigned>(kDeflateWindowSize - s.lookahead - s.strstart);

        // If the window is almost full and there is insufficient lookahead,
        // move the upper half to the lower one to make room in the upper half.
        if (s.strstart >= kDeflateWSize + kDeflateMaxDist) {
            std::memcpy(s.window, s.window + kDeflateWSize, kDeflateWSize - more);
            s.match_start -= kDeflateWSize;
            s.strstart -= kDeflateWSize; // we now have strstart >= MAX_DIST
            s.block_start -= static_cast<std::int32_t>(kDeflateWSize);
            if (s.insert > s.strstart) {
                s.insert = s.strstart;
            }
            slide_hash(s);
            more += kDeflateWSize;
        }
        if (in.avail == 0) {
            break;
        }

        n = read_buf(s, in, s.window + s.strstart + s.lookahead, more);
        s.lookahead += n;

        // Initialize the hash value now that we have some input:
        if (s.lookahead + s.insert >= kDeflateMinMatch) {
            std::uint32_t str = s.strstart - s.insert;
            s.ins_h = s.window[str];
            update_hash(s, s.ins_h, s.window[str + 1]);
            while (s.insert != 0) {
                update_hash(s, s.ins_h, s.window[str + kDeflateMinMatch - 1]);
                s.prev[str & kDeflateWMask] = s.head[s.ins_h];
                s.head[s.ins_h] = static_cast<DeflatePos>(str);
                str++;
                s.insert--;
                if (s.lookahead + s.insert < kDeflateMinMatch) {
                    break;
                }
            }
        }
        // If the whole input has less than MIN_MATCH bytes, ins_h is garbage,
        // but this is not important since only literal bytes will be emitted.
    } while (s.lookahead < kDeflateMinLookahead && in.avail != 0);

    // If the WIN_INIT bytes after the end of the current data have never been
    // written, zero those bytes so the longest-match routines never read
    // uninitialized memory. WIN_INIT == MAX_MATCH since matching may scan to
    // strstart + MAX_MATCH, ignoring lookahead.
    if (s.high_water < kDeflateWindowSize) {
        std::uint32_t curr = s.strstart + s.lookahead;
        std::uint32_t init;

        if (s.high_water < curr) {
            // Previous high water mark below current data -- zero WIN_INIT
            // bytes or up to end of window, whichever is less.
            init = static_cast<std::uint32_t>(kDeflateWindowSize) - curr;
            if (init > kDeflateWinInit) {
                init = kDeflateWinInit;
            }
            std::memset(s.window + curr, 0, init);
            s.high_water = curr + init;
        } else if (s.high_water < curr + kDeflateWinInit) {
            // High water mark at or above current data, but below current
            // data plus WIN_INIT -- zero out to current data plus WIN_INIT,
            // or up to end of window, whichever is less.
            init = curr + kDeflateWinInit - s.high_water;
            if (init > kDeflateWindowSize - s.high_water) {
                init = kDeflateWindowSize - s.high_water;
            }
            std::memset(s.window + s.high_water, 0, init);
            s.high_water += init;
        }
    }
}

// Set match_start to the longest match starting at the given string and
// return its length. Matches shorter or equal to prev_length are discarded,
// in which case the result is equal to prev_length and match_start is
// garbage. Portable variant (no UNALIGNED_OK two-byte compares).
std::uint32_t longest_match(DeflateState &s, DeflateIPos cur_match) noexcept {
    unsigned chain_length = s.max_chain_length; // max hash chain length
    const std::uint8_t *scan = s.window + s.strstart; // current string
    const std::uint8_t *match; // matched string
    int len; // length of current match
    int best_len = static_cast<int>(s.prev_length); // best match length so far
    int nice_match = s.nice_match; // stop if match long enough
    // Stop when cur_match becomes <= limit. To simplify the code, we prevent
    // matches with the string of window index 0.
    const DeflateIPos limit = s.strstart > kDeflateMaxDist ? s.strstart - kDeflateMaxDist : kDeflateNil;
    const DeflatePos *prev = s.prev;
    const std::uint32_t wmask = kDeflateWMask;
    const std::uint8_t *const strend = s.window + s.strstart + kDeflateMaxMatch;
    std::uint8_t scan_end1 = scan[best_len - 1];
    std::uint8_t scan_end = scan[best_len];

    // Do not waste too much time if we already have a good match:
    if (s.prev_length >= s.good_match) {
        chain_length >>= 2;
    }
    // Do not look for matches beyond the end of the input. This is necessary
    // to make deflate deterministic.
    if (static_cast<unsigned>(nice_match) > s.lookahead) {
        nice_match = static_cast<int>(s.lookahead);
    }

    do {
        match = s.window + cur_match;

        // Skip to next match if the match length cannot increase or if the
        // match length is less than 2. Note that the checks below for
        // insufficient lookahead only occur occasionally for performance
        // reasons. The length of the match is limited to the lookahead, so
        // the output is not affected by reads past it.
        if (match[best_len] != scan_end || match[best_len - 1] != scan_end1 || *match != *scan || *++match != scan[1]) {
            continue;
        }

        // The check at best_len - 1 can be removed because it will be made
        // again later. It is not necessary to compare scan[2] and match[2]
        // since they are always equal when the other bytes match, given that
        // the hash keys are equal and that HASH_BITS >= 8.
        scan += 2;
        match++;

        // We check for insufficient lookahead only every 8th comparison; the
        // 256th check will be made at strstart + 258.
        do {
        } while (*++scan == *++match && *++scan == *++match && *++scan == *++match && *++scan == *++match &&
                 *++scan == *++match && *++scan == *++match && *++scan == *++match && *++scan == *++match &&
                 scan < strend);

        len = static_cast<int>(kDeflateMaxMatch) - static_cast<int>(strend - scan);
        scan = strend - kDeflateMaxMatch;

        if (len > best_len) {
            s.match_start = cur_match;
            best_len = len;
            if (len >= nice_match) {
                break;
            }
            scan_end1 = scan[best_len - 1];
            scan_end = scan[best_len];
        }
    } while ((cur_match = prev[cur_match & wmask]) > limit && --chain_length != 0);

    if (static_cast<unsigned>(best_len) <= s.lookahead) {
        return static_cast<std::uint32_t>(best_len);
    }
    return s.lookahead;
}

// Compress as much as possible from the input stream. No lazy evaluation of
// matches; new strings are inserted in the dictionary only for unmatched
// strings or short matches. Used for levels 1..3.
BlockState deflate_fast(DeflateState &s, DeflateIn &in, DeflateOut &out, int flush) noexcept {
    DeflateIPos hash_head; // head of the hash chain
    bool bflush; // set if current block must be flushed

    for (;;) {
        // Make sure that we always have enough lookahead, except at the end
        // of the input file. We need MAX_MATCH bytes for the next match,
        // plus MIN_MATCH bytes to insert the string following the next match.
        if (s.lookahead < kDeflateMinLookahead) {
            fill_window(s, in);
            if (s.lookahead < kDeflateMinLookahead && flush == kFlushNone) {
                return BlockState::NeedMore;
            }
            if (s.lookahead == 0) {
                break; // flush the current block
            }
        }

        // Insert the string window[strstart .. strstart + 2] in the
        // dictionary, and set hash_head to the head of the hash chain:
        hash_head = kDeflateNil;
        if (s.lookahead >= kDeflateMinMatch) {
            hash_head = insert_string(s, s.strstart);
        }

        // Find the longest match, discarding those <= prev_length. At this
        // point we have always match_length < MIN_MATCH.
        if (hash_head != kDeflateNil && s.strstart - hash_head <= kDeflateMaxDist) {
            // To simplify the code, we prevent matches with the string of
            // window index 0 (in particular we have to avoid a match of the
            // string with itself at the start of the input file).
            s.match_length = longest_match(s, hash_head);
            // longest_match() sets match_start
        }
        if (s.match_length >= kDeflateMinMatch) {
            deflate_tally_dist(s, s.strstart - s.match_start,
                               static_cast<std::uint8_t>(s.match_length - kDeflateMinMatch), bflush);

            s.lookahead -= s.match_length;

            // Insert new strings in the hash table only if the match length
            // is not too large. This saves time but degrades compression.
            if (s.match_length <= s.max_lazy_match && s.lookahead >= kDeflateMinMatch) {
                s.match_length--; // string at strstart already in table
                do {
                    s.strstart++;
                    hash_head = insert_string(s, s.strstart);
                    // strstart never exceeds WSIZE-MAX_MATCH, so there are
                    // always MIN_MATCH bytes ahead.
                } while (--s.match_length != 0);
                s.strstart++;
            } else {
                s.strstart += s.match_length;
                s.match_length = 0;
                s.ins_h = s.window[s.strstart];
                update_hash(s, s.ins_h, s.window[s.strstart + 1]);
                // If lookahead < MIN_MATCH, ins_h is garbage, but it does not
                // matter since it will be recomputed at next deflate call.
            }
        } else {
            // No match, output a literal byte
            deflate_tally_lit(s, s.window[s.strstart], bflush);
            s.lookahead--;
            s.strstart++;
        }
        if (bflush) {
            FIBER_DEFLATE_FLUSH_BLOCK(&s, out, false);
        }
    }
    s.insert = s.strstart < kDeflateMinMatch - 1 ? s.strstart : kDeflateMinMatch - 1;
    if (flush == kFlushFinish) {
        FIBER_DEFLATE_FLUSH_BLOCK(&s, out, true);
        return BlockState::FinishDone;
    }
    if (s.sym_next != 0) {
        FIBER_DEFLATE_FLUSH_BLOCK(&s, out, false);
    }
    return BlockState::BlockDone;
}

// Same as above, but achieves better compression (levels 4..9) through lazy
// match evaluation: a match is finally adopted only if there is no better
// match at the next window position.
BlockState deflate_slow(DeflateState &s, DeflateIn &in, DeflateOut &out, int flush) noexcept {
    DeflateIPos hash_head; // head of hash chain
    bool bflush; // set if current block must be flushed

    // Process the input block.
    for (;;) {
        // Make sure that we always have enough lookahead, except at the end
        // of the input file. We need MAX_MATCH bytes for the next match,
        // plus MIN_MATCH bytes to insert the string following the next match.
        if (s.lookahead < kDeflateMinLookahead) {
            fill_window(s, in);
            if (s.lookahead < kDeflateMinLookahead && flush == kFlushNone) {
                return BlockState::NeedMore;
            }
            if (s.lookahead == 0) {
                break; // flush the current block
            }
        }

        // Insert the string window[strstart .. strstart + 2] in the
        // dictionary, and set hash_head to the head of the hash chain:
        hash_head = kDeflateNil;
        if (s.lookahead >= kDeflateMinMatch) {
            hash_head = insert_string(s, s.strstart);
        }

        // Find the longest match, discarding those <= prev_length.
        s.prev_length = s.match_length;
        s.prev_match = s.match_start;
        s.match_length = kDeflateMinMatch - 1;

        if (hash_head != kDeflateNil && s.prev_length < s.max_lazy_match && s.strstart - hash_head <= kDeflateMaxDist) {
            // To simplify the code, we prevent matches with the string of
            // window index 0 (in particular we have to avoid a match of the
            // string with itself at the start of the input file).
            s.match_length = longest_match(s, hash_head);
            // longest_match() sets match_start

            if (s.match_length <= 5 && s.match_length == kDeflateMinMatch &&
                s.strstart - s.match_start > kDeflateTooFar) {
                // If prev_match is also MIN_MATCH, match_start is garbage but
                // we will ignore the current match anyway.
                s.match_length = kDeflateMinMatch - 1;
            }
        }
        // If there was a match at the previous step and the current match is
        // not better, output the previous match:
        if (s.prev_length >= kDeflateMinMatch && s.match_length <= s.prev_length) {
            const std::uint32_t max_insert = s.strstart + s.lookahead - kDeflateMinMatch;
            // Do not insert strings in hash table beyond this.

            deflate_tally_dist(s, s.strstart - 1 - s.prev_match,
                               static_cast<std::uint8_t>(s.prev_length - kDeflateMinMatch), bflush);

            // Insert in hash table all strings up to the end of the match.
            // strstart - 1 and strstart are already inserted. If there is not
            // enough lookahead, the last two strings are not inserted in the
            // hash table.
            s.lookahead -= s.prev_length - 1;
            s.prev_length -= 2;
            do {
                if (++s.strstart <= max_insert) {
                    hash_head = insert_string(s, s.strstart);
                }
            } while (--s.prev_length != 0);
            s.match_available = false;
            s.match_length = kDeflateMinMatch - 1;
            s.strstart++;

            if (bflush) {
                FIBER_DEFLATE_FLUSH_BLOCK(&s, out, false);
            }
        } else if (s.match_available) {
            // If there was no match at the previous position, output a single
            // literal. If there was a match but the current match is longer,
            // truncate the previous match to a single literal.
            deflate_tally_lit(s, s.window[s.strstart - 1], bflush);
            if (bflush) {
                FIBER_DEFLATE_FLUSH_BLOCK_ONLY(&s, out, false);
            }
            s.strstart++;
            s.lookahead--;
            if (out.avail == 0) {
                return BlockState::NeedMore;
            }
        } else {
            // There is no previous match to compare with, wait for the next
            // step to decide.
            s.match_available = true;
            s.strstart++;
            s.lookahead--;
        }
    }
    if (s.match_available) {
        deflate_tally_lit(s, s.window[s.strstart - 1], bflush);
        s.match_available = false;
    }
    s.insert = s.strstart < kDeflateMinMatch - 1 ? s.strstart : kDeflateMinMatch - 1;
    if (flush == kFlushFinish) {
        FIBER_DEFLATE_FLUSH_BLOCK(&s, out, true);
        return BlockState::FinishDone;
    }
    if (s.sym_next != 0) {
        FIBER_DEFLATE_FLUSH_BLOCK(&s, out, false);
    }
    return BlockState::BlockDone;
}

// The zlib deflate() driver loop for one caller operation. Guards and wrapper
// features are dropped (status validation, header/trailer writing, partial/
// full/block flush kinds); the sequencing of pending drains, duplicate-flush
// suppression and the finish re-entry guard is upstream's.
RunResult deflate_run(DeflateState &s, DeflateIn &in, DeflateOut &out, int flush) noexcept {
    const int old_flush = s.last_flush;
    s.last_flush = flush;

    // Flush as much pending output as possible.
    if (s.pending != 0) {
        flush_pending(s, out);
        if (out.avail == 0) {
            // Since avail is 0, the operation will be repeated with more
            // output space, but possibly with both pending and input empty.
            // There won't be anything to do, but this is not an error, so
            // make sure we don't report "nothing to do" on the next call:
            s.last_flush = -1;
            return RunResult::Ok;
        }

        // Make sure there is something to do and avoid duplicate consecutive
        // flushes. For repeated finish calls we keep returning the stream end
        // instead.
    } else if (in.avail == 0 && deflate_rank(flush) <= deflate_rank(old_flush) && flush != kFlushFinish) {
        return RunResult::NothingToDo;
    }

    // Start a new block or continue the current one. The finish-started
    // guard replaces upstream's FINISH_STATE re-entry check.
    if (in.avail != 0 || s.lookahead != 0 || (flush != kFlushNone && !s.finish_started())) {
        BlockState bstate = s.level <= 3 ? deflate_fast(s, in, out, flush) : deflate_slow(s, in, out, flush);

        if (bstate == BlockState::FinishStarted || bstate == BlockState::FinishDone) {
            s.status = DeflateStreamStatus::FinishStarted;
        }
        if (bstate == BlockState::NeedMore || bstate == BlockState::FinishStarted) {
            if (out.avail == 0) {
                s.last_flush = -1; // avoid "nothing to do" next call, see above
            }
            return RunResult::Ok;
            // If flush != none and avail == 0, the next call should use the
            // same flush parameter to make sure that the flush is complete.
            // We don't have to output an empty block here; this will be done
            // at next call. This also ensures that for a very small output
            // buffer, we emit at most one empty block.
        }
        if (bstate == BlockState::BlockDone) {
            // The only flush kinds left are sync (emit the marker) and none
            // (unreachable: a driver only returns BlockDone for a flush).
            deflate_tr_stored_block(s, nullptr, 0, 0);
            s.flush_marker_owed = false;
            flush_pending(s, out);
            if (out.avail == 0) {
                s.last_flush = -1; // avoid "nothing to do" next call, see above
                return RunResult::Ok;
            }
        }
    }

    if (flush != kFlushFinish) {
        return RunResult::Ok;
    }
    // Raw stream: no trailer, the caller (GzipEncoder) emits it.
    return RunResult::StreamEnd;
}

// Initialize the match machinery for a new stream (zlib lm_init).
void lm_init(DeflateState &s) noexcept {
    clear_hash(s);

    // Set the default configuration parameters:
    const DeflateConfig &cfg = kConfigTable[s.level];
    s.max_lazy_match = cfg.max_lazy;
    s.good_match = cfg.good_length;
    s.nice_match = static_cast<std::int32_t>(cfg.nice_length);
    s.max_chain_length = cfg.max_chain;

    s.strstart = 0;
    s.block_start = 0;
    s.lookahead = 0;
    s.insert = 0;
    s.match_length = s.prev_length = kDeflateMinMatch - 1;
    s.match_available = false;
    s.ins_h = 0;
}

} // namespace

void DeflateEncoder::init(DeflateState &state, void *workspace, int level) noexcept {
    FIBER_ASSERT(level >= 1 && level <= 9);
    FIBER_ASSERT(workspace != nullptr);

    state = DeflateState{};
    auto *base = static_cast<std::uint8_t *>(workspace);
    state.window = base;
    base += kDeflateWindowBytes;
    state.prev = reinterpret_cast<DeflatePos *>(base);
    base += kDeflatePrevBytes;
    state.head = reinterpret_cast<DeflatePos *>(base);
    base += kDeflateHeadBytes;
    state.pending_buf = base;
    state.pending_out = state.pending_buf;

    state.level = level;

    deflate_tr_init(state);
    lm_init(state);
}

EncodeStep DeflateEncoder::write(DeflateState &state, std::span<const std::uint8_t> input,
                                 std::span<std::uint8_t> output) noexcept {
    FIBER_ASSERT(!state.finish_started());

    DeflateIn in{input.data(), input.size()};
    DeflateOut out{output.data(), output.size()};

    if (in.avail == 0) {
        // An empty write must not touch the stream state (upstream would
        // treat it as a duplicate no-op flush); only drain pending output.
        if (state.pending != 0) {
            flush_pending(state, out);
        }
        return EncodeStep{0, out.total, state.pending != 0 ? EncodeStatus::NeedOutput : EncodeStatus::NeedInput};
    }

    const RunResult result = deflate_run(state, in, out, kFlushNone);
    FIBER_ASSERT(result == RunResult::Ok);

    return EncodeStep{input.size() - in.avail, out.total,
                      in.avail != 0 ? EncodeStatus::NeedOutput : EncodeStatus::NeedInput};
}

EncodeStep DeflateEncoder::flush(DeflateState &state, std::span<std::uint8_t> output) noexcept {
    FIBER_ASSERT(!state.finish_started());

    DeflateIn in{nullptr, 0};
    DeflateOut out{output.data(), output.size()};

    if (!state.sync_flush_active) {
        state.sync_flush_active = true;
        state.flush_marker_owed = true;
    }
    const RunResult result = deflate_run(state, in, out, kFlushSync);
    FIBER_ASSERT(result != RunResult::StreamEnd);

    EncodeStep step;
    step.written = out.total;
    if (result == RunResult::NothingToDo || (state.pending == 0 && !state.flush_marker_owed)) {
        // A repeated flush with nothing new since the previous one, or the
        // marker is out and drained.
        state.sync_flush_active = false;
        state.flush_marker_owed = false;
        step.status = EncodeStatus::Flushed;
    } else {
        step.status = EncodeStatus::NeedOutput;
    }
    return step;
}

EncodeStep DeflateEncoder::finish(DeflateState &state, std::span<std::uint8_t> output) noexcept {
    DeflateIn in{nullptr, 0};
    DeflateOut out{output.data(), output.size()};

    const RunResult result = deflate_run(state, in, out, kFlushFinish);

    EncodeStep step;
    step.written = out.total;
    step.status = result == RunResult::StreamEnd ? EncodeStatus::Finished : EncodeStatus::NeedOutput;
    return step;
}

} // namespace fiber::compression
