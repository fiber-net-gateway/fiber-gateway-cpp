// Huffman block emission for the private DEFLATE encoder, ported from zlib
// 1.3.2 trees.c (Jean-loup Gailly; see src/compression/UPSTREAM.md). Removed
// upstream features: ZLIB_DEBUG tracing, the Z_FIXED strategy branch,
// detect_data_type (data_type was only observable through removed APIs), the
// level-0 stored-only path, and LIT_MEM (this port always uses the three-byte
// sym_buf overlay). Everything else — heap construction, bit-length overflow
// repair, canonical code generation, tree scanning/serialization, block cost
// comparison — is a line-for-line port operating on DeflateState.

#include "DeflateState.h"

#include <cstring>

namespace fiber::compression {

namespace {

// Bit accumulator width (deflate.h Buf_size).
inline constexpr int kBufSize = 16;

// Bit length codes must not exceed 7 bits (trees.c MAX_BL_BITS).
inline constexpr int kMaxBlBits = 7;

// Repeat codes for the bit length tree (trees.c).
inline constexpr int kRep3_6 = 16; // repeat previous bit length 3-6 times
inline constexpr int kRepz3_10 = 17; // repeat a zero length 3-10 times
inline constexpr int kRepz11_138 = 18; // repeat a zero length 11-138 times

// Block types (deflate.h).
inline constexpr int kStoredBlock = 0;
inline constexpr int kStaticTrees = 1;
inline constexpr int kDynTrees = 2;

// Index within the heap array of the least frequent node (trees.c SMALLEST).
inline constexpr int kSmallest = 1;

inline void put_byte(DeflateState &s, std::uint8_t b) noexcept { s.pending_buf[s.pending++] = b; }

// Output a short LSB first. Invariant (upstream): there is enough room in
// pending_buf.
inline void put_short(DeflateState &s, std::uint16_t w) noexcept {
    put_byte(s, static_cast<std::uint8_t>(w & 0xff));
    put_byte(s, static_cast<std::uint8_t>(w >> 8));
}

// Send a value on a given number of bits. Invariant: 0 < length <= 16 and
// value fits in length bits.
inline void send_bits(DeflateState &s, int value, int length) noexcept {
    if (s.bi_valid > kBufSize - length) {
        s.bi_buf |= static_cast<std::uint16_t>(static_cast<unsigned>(value) << s.bi_valid);
        put_short(s, s.bi_buf);
        s.bi_buf = static_cast<std::uint16_t>(static_cast<unsigned>(value) >> (kBufSize - s.bi_valid));
        s.bi_valid += length - kBufSize;
    } else {
        s.bi_buf |= static_cast<std::uint16_t>(static_cast<unsigned>(value) << s.bi_valid);
        s.bi_valid += length;
    }
}

// Send a code of the given tree.
inline void send_code(DeflateState &s, int c, const DeflateCtData *tree) noexcept {
    send_bits(s, tree[c].code, tree[c].len);
}

// Flush the bit buffer, keeping at most 7 bits in it.
void bi_flush(DeflateState &s) noexcept {
    if (s.bi_valid == 16) {
        put_short(s, s.bi_buf);
        s.bi_buf = 0;
        s.bi_valid = 0;
    } else if (s.bi_valid >= 8) {
        put_byte(s, static_cast<std::uint8_t>(s.bi_buf));
        s.bi_buf >>= 8;
        s.bi_valid -= 8;
    }
}

// Flush the bit buffer and align the output on a byte boundary.
void bi_windup(DeflateState &s) noexcept {
    if (s.bi_valid > 8) {
        put_short(s, s.bi_buf);
    } else if (s.bi_valid > 0) {
        put_byte(s, static_cast<std::uint8_t>(s.bi_buf));
    }
    s.bi_buf = 0;
    s.bi_valid = 0;
}

// Initialize a new block.
void init_block(DeflateState &s) noexcept {
    for (int n = 0; n < static_cast<int>(kDeflateLCodes); n++) {
        s.dyn_ltree[n].freq = 0;
    }
    for (int n = 0; n < static_cast<int>(kDeflateDCodes); n++) {
        s.dyn_dtree[n].freq = 0;
    }
    for (int n = 0; n < static_cast<int>(kDeflateBCodes); n++) {
        s.bl_tree[n].freq = 0;
    }

    s.dyn_ltree[kDeflateEndBlock].freq = 1;
    s.opt_len = s.static_len = 0;
    s.sym_next = 0;
}

// Compare two subtrees, using the tree depth as tie breaker when the
// subtrees have equal frequency. This minimizes the worst case length.
inline bool smaller(const DeflateCtData *tree, int n, int m, const std::uint8_t *depth) noexcept {
    return tree[n].freq < tree[m].freq || (tree[n].freq == tree[m].freq && depth[n] <= depth[m]);
}

// Restore the heap property by moving down the tree starting at node k,
// exchanging a node with the smallest of its two sons if necessary.
void pqdownheap(DeflateState &s, DeflateCtData *tree, int k) noexcept {
    const int v = s.heap[k];
    int j = k << 1; // left son of k
    while (j <= s.heap_len) {
        // Set j to the smallest of the two sons.
        if (j < s.heap_len && smaller(tree, s.heap[j + 1], s.heap[j], s.depth)) {
            j++;
        }
        // Exit if v is smaller than both sons.
        if (smaller(tree, v, s.heap[j], s.depth)) {
            break;
        }

        // Exchange v with the smallest son.
        s.heap[k] = s.heap[j];
        k = j;

        // And continue down the tree, setting j to the left son of k.
        j <<= 1;
    }
    s.heap[k] = v;
}

// Remove the smallest element from the heap and recreate the heap with one
// less element.
inline int pqremove(DeflateState &s, DeflateCtData *tree) noexcept {
    const int top = s.heap[kSmallest];
    s.heap[kSmallest] = s.heap[s.heap_len--];
    pqdownheap(s, tree, kSmallest);
    return top;
}

// Compute the optimal bit lengths for a tree and update the total bit length
// for the current block. Invariant: freq and dad are set, heap[heap_max..]
// are the tree nodes sorted by increasing frequency.
void gen_bitlen(DeflateState &s, DeflateTreeDesc *desc) noexcept {
    DeflateCtData *tree = desc->dyn_tree;
    const int max_code = desc->max_code;
    const DeflateCtData *stree = desc->stat_desc->static_tree;
    const std::uint8_t *extra = desc->stat_desc->extra_bits;
    const int base = desc->stat_desc->extra_base;
    const int max_length = desc->stat_desc->max_length;
    int h; // heap index
    int n, m; // iterate over the tree elements
    int bits; // bit length
    int xbits; // extra bits
    std::uint16_t f; // frequency
    int overflow = 0; // number of elements with bit length too large

    for (bits = 0; bits <= static_cast<int>(kDeflateMaxBits); bits++) {
        s.bl_count[bits] = 0;
    }

    // In a first pass, compute the optimal bit lengths (which may overflow in
    // the case of the bit length tree).
    tree[s.heap[s.heap_max]].len = 0; // root of the heap

    for (h = s.heap_max + 1; h < static_cast<int>(kDeflateHeapSize); h++) {
        n = s.heap[h];
        bits = tree[tree[n].dad].len + 1;
        if (bits > max_length) {
            bits = max_length;
            overflow++;
        }
        tree[n].len = static_cast<std::uint16_t>(bits);
        // tree[n].dad is no longer needed and has been overwritten by len.

        if (n > max_code) {
            continue; // not a leaf node
        }

        s.bl_count[bits]++;
        xbits = 0;
        if (n >= base) {
            xbits = extra[n - base];
        }
        f = tree[n].freq;
        s.opt_len += static_cast<std::uint64_t>(f) * static_cast<unsigned>(bits + xbits);
        if (stree != nullptr) {
            s.static_len += static_cast<std::uint64_t>(f) * static_cast<unsigned>(stree[n].len + xbits);
        }
    }
    if (overflow == 0) {
        return;
    }

    // This happens for example on obj2 and pic of the Calgary corpus.
    // Find the first bit length which could increase:
    do {
        bits = max_length - 1;
        while (s.bl_count[bits] == 0) {
            bits--;
        }
        s.bl_count[bits]--; // move one leaf down the tree
        s.bl_count[bits + 1] += 2; // move one overflow item as its brother
        s.bl_count[max_length]--;
        // The brother of the overflow item also moves one step up, but this
        // does not affect bl_count[max_length].
        overflow -= 2;
    } while (overflow > 0);

    // Now recompute all bit lengths, scanning in increasing frequency. h is
    // still equal to HEAP_SIZE.
    for (bits = max_length; bits != 0; bits--) {
        n = s.bl_count[bits];
        while (n != 0) {
            m = s.heap[--h];
            if (m > max_code) {
                continue;
            }
            if (tree[m].len != static_cast<std::uint16_t>(bits)) {
                s.opt_len += (static_cast<std::uint64_t>(bits) - tree[m].len) * tree[m].freq;
                tree[m].len = static_cast<std::uint16_t>(bits);
            }
            n--;
        }
    }
}

// Construct one Huffman tree and assign the code bit strings and lengths.
// Invariant: the field freq is set for all tree elements.
void build_tree(DeflateState &s, DeflateTreeDesc *desc) noexcept {
    DeflateCtData *tree = desc->dyn_tree;
    const DeflateCtData *stree = desc->stat_desc->static_tree;
    const int elems = desc->stat_desc->elems;
    int n, m; // iterate over heap elements
    int max_code = -1; // largest code with non zero frequency
    int node; // new node being created

    // Construct the initial heap, with least frequent element in heap[1]. The
    // sons of heap[n] are heap[2*n] and heap[2*n+1]. heap[0] is not used.
    s.heap_len = 0;
    s.heap_max = static_cast<int>(kDeflateHeapSize);

    for (n = 0; n < elems; n++) {
        if (tree[n].freq != 0) {
            s.heap[++(s.heap_len)] = max_code = n;
            s.depth[n] = 0;
        } else {
            tree[n].len = 0;
        }
    }

    // The pkzip format requires that at least one distance code exists, and
    // that at least one bit should be sent even if there is only one possible
    // code. So to avoid special checks later on we force at least two codes
    // of non zero frequency.
    while (s.heap_len < 2) {
        node = s.heap[++(s.heap_len)] = (max_code < 2 ? ++max_code : 0);
        tree[node].freq = 1;
        s.depth[node] = 0;
        s.opt_len--;
        if (stree != nullptr) {
            s.static_len -= stree[node].len;
        }
        // node is 0 or 1 so it does not have extra bits
    }
    desc->max_code = max_code;

    // The elements heap[heap_len/2 + 1 .. heap_len] are leaves of the tree,
    // establish sub-heaps of increasing lengths:
    for (n = s.heap_len / 2; n >= 1; n--) {
        pqdownheap(s, tree, n);
    }

    // Construct the Huffman tree by repeatedly combining the least two
    // frequent nodes.
    node = elems; // next internal node of the tree
    do {
        n = pqremove(s, tree); // n = node of least frequency
        m = s.heap[kSmallest]; // m = node of next least frequency

        // Keep the nodes sorted by frequency.
        s.heap[--(s.heap_max)] = n;
        s.heap[--(s.heap_max)] = m;

        // Create a new node father of n and m.
        tree[node].freq = static_cast<std::uint16_t>(tree[n].freq + tree[m].freq);
        s.depth[node] = static_cast<std::uint8_t>((s.depth[n] >= s.depth[m] ? s.depth[n] : s.depth[m]) + 1);
        tree[n].dad = tree[m].dad = static_cast<std::uint16_t>(node);

        // And insert the new node in the heap.
        s.heap[kSmallest] = node++;
        pqdownheap(s, tree, kSmallest);
    } while (s.heap_len >= 2);

    s.heap[--(s.heap_max)] = s.heap[kSmallest];

    // At this point, the fields freq and dad are set. We can now generate the
    // bit lengths, then the bit codes.
    gen_bitlen(s, desc);
    deflate_tables::gen_codes(tree, max_code, s.bl_count);
}

// Scan a literal or distance tree to determine the frequencies of the codes
// in the bit length tree.
void scan_tree(DeflateState &s, DeflateCtData *tree, int max_code) noexcept {
    int n; // iterates over all tree elements
    int prevlen = -1; // last emitted length
    int curlen; // length of current code
    int nextlen = tree[0].len; // length of next code
    int count = 0; // repeat count of the current code
    int max_count = 7; // max repeat count
    int min_count = 4; // min repeat count

    if (nextlen == 0) {
        max_count = 138;
        min_count = 3;
    }
    tree[max_code + 1].len = static_cast<std::uint16_t>(0xffff); // guard

    for (n = 0; n <= max_code; n++) {
        curlen = nextlen;
        nextlen = tree[n + 1].len;
        if (++count < max_count && curlen == nextlen) {
            continue;
        } else if (count < min_count) {
            s.bl_tree[curlen].freq = static_cast<std::uint16_t>(s.bl_tree[curlen].freq + count);
        } else if (curlen != 0) {
            if (curlen != prevlen) {
                s.bl_tree[curlen].freq++;
            }
            s.bl_tree[kRep3_6].freq++;
        } else if (count <= 10) {
            s.bl_tree[kRepz3_10].freq++;
        } else {
            s.bl_tree[kRepz11_138].freq++;
        }
        count = 0;
        prevlen = curlen;
        if (nextlen == 0) {
            max_count = 138;
            min_count = 3;
        } else if (curlen == nextlen) {
            max_count = 6;
            min_count = 3;
        } else {
            max_count = 7;
            min_count = 4;
        }
    }
}

// Send a literal or distance tree in compressed form, using the codes in
// bl_tree.
void send_tree(DeflateState &s, DeflateCtData *tree, int max_code) noexcept {
    int n; // iterates over all tree elements
    int prevlen = -1; // last emitted length
    int curlen; // length of current code
    int nextlen = tree[0].len; // length of next code
    int count = 0; // repeat count of the current code
    int max_count = 7; // max repeat count
    int min_count = 4; // min repeat count

    // guard already set by scan_tree
    if (nextlen == 0) {
        max_count = 138;
        min_count = 3;
    }

    for (n = 0; n <= max_code; n++) {
        curlen = nextlen;
        nextlen = tree[n + 1].len;
        if (++count < max_count && curlen == nextlen) {
            continue;
        } else if (count < min_count) {
            do {
                send_code(s, curlen, s.bl_tree);
            } while (--count != 0);
        } else if (curlen != 0) {
            if (curlen != prevlen) {
                send_code(s, curlen, s.bl_tree);
                count--;
            }
            send_code(s, kRep3_6, s.bl_tree);
            send_bits(s, count - 3, 2);
        } else if (count <= 10) {
            send_code(s, kRepz3_10, s.bl_tree);
            send_bits(s, count - 3, 3);
        } else {
            send_code(s, kRepz11_138, s.bl_tree);
            send_bits(s, count - 11, 7);
        }
        count = 0;
        prevlen = curlen;
        if (nextlen == 0) {
            max_count = 138;
            min_count = 3;
        } else if (curlen == nextlen) {
            max_count = 6;
            min_count = 3;
        } else {
            max_count = 7;
            min_count = 4;
        }
    }
}

// Construct the Huffman tree for the bit lengths and return the index in
// bl_order of the last bit length code to send.
int build_bl_tree(DeflateState &s) noexcept {
    int max_blindex; // index of last bit length code of non zero freq

    // Determine the bit length frequencies for literal and distance trees.
    scan_tree(s, s.dyn_ltree, s.l_desc.max_code);
    scan_tree(s, s.dyn_dtree, s.d_desc.max_code);

    // Build the bit length tree: opt_len now includes the length of the tree
    // representations, except the lengths of the bit lengths codes and the
    // 5 + 5 + 4 bits for the counts.
    build_tree(s, &s.bl_desc);

    // Determine the number of bit length codes to send. The pkzip format
    // requires that at least 4 bit length codes be sent.
    for (max_blindex = static_cast<int>(kDeflateBCodes) - 1; max_blindex >= 3; max_blindex--) {
        if (s.bl_tree[kDeflateBlOrder[max_blindex]].len != 0) {
            break;
        }
    }
    // Update opt_len to include the bit length tree and counts.
    s.opt_len += 3 * (static_cast<std::uint64_t>(max_blindex) + 1) + 5 + 5 + 4;

    return max_blindex;
}

// Send the header for a block using dynamic Huffman trees.
void send_all_trees(DeflateState &s, int lcodes, int dcodes, int blcodes) noexcept {
    int rank; // index in bl_order

    send_bits(s, lcodes - 257, 5); // not +255 as stated in appnote.txt
    send_bits(s, dcodes - 1, 5);
    send_bits(s, blcodes - 4, 4); // not -3 as stated in appnote.txt
    for (rank = 0; rank < blcodes; rank++) {
        send_bits(s, s.bl_tree[kDeflateBlOrder[rank]].len, 3);
    }

    send_tree(s, s.dyn_ltree, lcodes - 1); // literal tree
    send_tree(s, s.dyn_dtree, dcodes - 1); // distance tree
}

// Send the block data compressed using the given Huffman trees.
void compress_block(DeflateState &s, const DeflateCtData *ltree, const DeflateCtData *dtree) noexcept {
    unsigned dist; // distance of matched string
    int lc; // match length or unmatched char (if dist == 0)
    unsigned sx = 0; // running index in the symbol buffer
    unsigned code; // the code to send
    int extra; // number of extra bits to send

    if (s.sym_next != 0) {
        do {
            dist = s.sym_buf()[sx++] & 0xff;
            dist += static_cast<unsigned>(s.sym_buf()[sx++] & 0xff) << 8;
            lc = s.sym_buf()[sx++];
            if (dist == 0) {
                send_code(s, lc, ltree); // send a literal byte
            } else {
                // Here, lc is the match length - MIN_MATCH.
                code = kDeflateLengthCode[lc];
                send_code(s, static_cast<int>(code) + kDeflateLiterals + 1, ltree); // length code
                extra = kDeflateExtraLBits[code];
                if (extra != 0) {
                    lc -= static_cast<int>(kDeflateBaseLength[code]);
                    send_bits(s, lc, extra); // send the extra length bits
                }
                dist--; // dist is now the match distance - 1
                code = deflate_d_code(dist);

                send_code(s, static_cast<int>(code), dtree); // distance code
                extra = kDeflateExtraDBits[code];
                if (extra != 0) {
                    dist -= kDeflateBaseDist[code];
                    send_bits(s, static_cast<int>(dist), extra); // send the extra bits
                }
            }
        } while (sx < s.sym_next);
    }

    send_code(s, kDeflateEndBlock, ltree);
}

} // namespace

void deflate_tr_init(DeflateState &s) noexcept {
    s.l_desc.dyn_tree = s.dyn_ltree;
    s.l_desc.stat_desc = &kDeflateStaticLDesc;

    s.d_desc.dyn_tree = s.dyn_dtree;
    s.d_desc.stat_desc = &kDeflateStaticDDesc;

    s.bl_desc.dyn_tree = s.bl_tree;
    s.bl_desc.stat_desc = &kDeflateStaticBLDesc;

    s.bi_buf = 0;
    s.bi_valid = 0;

    // Initialize the first block of the stream.
    init_block(s);
}

void deflate_tr_stored_block(DeflateState &s, const std::uint8_t *buf, std::uint64_t stored_len, int last) noexcept {
    send_bits(s, (kStoredBlock << 1) + last, 3); // send block type
    bi_windup(s); // align on byte boundary
    put_short(s, static_cast<std::uint16_t>(stored_len));
    put_short(s, static_cast<std::uint16_t>(~stored_len));
    if (stored_len != 0) {
        std::memcpy(s.pending_buf + s.pending, buf, stored_len);
    }
    s.pending += static_cast<std::uint32_t>(stored_len);
}

void deflate_tr_flush_bits(DeflateState &s) noexcept { bi_flush(s); }

void deflate_tr_align(DeflateState &s) noexcept {
    send_bits(s, kStaticTrees << 1, 3);
    send_code(s, kDeflateEndBlock, kDeflateStaticLTree);
    bi_flush(s);
}

void deflate_tr_flush_block(DeflateState &s, const std::uint8_t *buf, std::uint64_t stored_len, int last) noexcept {
    std::uint64_t opt_lenb, static_lenb; // opt_len and static_len in bytes
    int max_blindex = 0; // index of last bit length code of non zero freq

    // Build the Huffman trees (levels are always >= 1 in this port).
    build_tree(s, &(s.l_desc));
    build_tree(s, &(s.d_desc));
    // At this point, opt_len and static_len are the total bit lengths of the
    // compressed block data, excluding the tree representations.

    // Build the bit length tree for the above two trees, and get the index in
    // bl_order of the last bit length code to send.
    max_blindex = build_bl_tree(s);

    // Determine the best encoding. Compute the block lengths in bytes.
    opt_lenb = (s.opt_len + 3 + 7) >> 3;
    static_lenb = (s.static_len + 3 + 7) >> 3;
    if (static_lenb <= opt_lenb) {
        opt_lenb = static_lenb;
    }

    if (stored_len + 4 <= opt_lenb && buf != nullptr) {
        // 4: two words for the lengths. The buf test is only necessary if
        // LIT_BUFSIZE > WSIZE; it is never too late to transform a block into
        // a stored block otherwise.
        deflate_tr_stored_block(s, buf, stored_len, last);
    } else if (static_lenb == opt_lenb) {
        send_bits(s, (kStaticTrees << 1) + last, 3);
        compress_block(s, kDeflateStaticLTree, kDeflateStaticDTree);
    } else {
        send_bits(s, (kDynTrees << 1) + last, 3);
        send_all_trees(s, s.l_desc.max_code + 1, s.d_desc.max_code + 1, max_blindex + 1);
        compress_block(s, s.dyn_ltree, s.dyn_dtree);
    }
    init_block(s);

    if (last != 0) {
        bi_windup(s);
    }
}

} // namespace fiber::compression
