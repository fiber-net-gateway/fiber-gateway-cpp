// Compiles a regexp tree to a Program. Ported from RE2's compile.cc (see
// UPSTREAM.md), where the idea of patching lists of unfilled arrows comes from
// http://swtch.com/~rsc/regexp/regexp1.html.
#include "regex/Compiler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fiber/common/Assert.h>

#include "regex/Rune.h"
#include "regex/Sparse.h"

namespace fiber::regex::detail {

namespace {

// List of instruction arrows that still need a target. Because they are not
// filled in yet, the arrow itself holds the "next" link. head refers to the out
// of inst[head >> 1] (head & 1 == 0) or its out1 (head & 1 == 1), and 0 is the
// empty list: instruction 0 is Fail, which never appears on a list.
struct PatchList {
    std::uint32_t head = 0;
    std::uint32_t tail = 0; // for constant-time append
};

constexpr PatchList make_patch_list(std::uint32_t p) noexcept { return PatchList{.head = p, .tail = p}; }

void patch(std::vector<Inst> &inst, PatchList list, std::uint32_t target) noexcept {
    while (list.head != 0) {
        Inst &ip = inst[list.head >> 1U];
        if ((list.head & 1U) != 0) {
            list.head = ip.out1();
            ip.set_out1(target);
        } else {
            list.head = ip.out();
            ip.set_out(target);
        }
    }
}

PatchList append(std::vector<Inst> &inst, PatchList l1, PatchList l2) noexcept {
    if (l1.head == 0) {
        return l2;
    }
    if (l2.head == 0) {
        return l1;
    }
    Inst &ip = inst[l1.tail >> 1U];
    if ((l1.tail & 1U) != 0) {
        ip.set_out1(l2.head);
    } else {
        ip.set_out(l2.head);
    }
    return PatchList{.head = l1.head, .tail = l2.tail};
}

// A compiled fragment. begin == 0 means it can never match.
struct Frag {
    Frag() = default;
    Frag(std::uint32_t begin_inst, PatchList end_list, bool can_be_empty) noexcept :
        begin(begin_inst), end(end_list), nullable(can_be_empty) {}

    std::uint32_t begin = 0;
    PatchList end{};
    bool nullable = false;
};

[[nodiscard]] constexpr bool is_no_match(const Frag &frag) noexcept { return frag.begin == 0; }

// Largest rune expressible in a UTF-8 sequence of `length` bytes.
[[nodiscard]] constexpr Rune max_rune(std::size_t length) noexcept {
    const std::size_t bits = length == 1 ? 7 : 8 - (length + 1) + (6 * (length - 1));
    return static_cast<Rune>((std::uint32_t{1} << bits) - 1U);
}

std::uint64_t next_program_id() noexcept {
    static std::atomic<std::uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

[[nodiscard]] constexpr std::uint64_t rune_cache_key(std::uint8_t lo, std::uint8_t hi, bool foldcase,
                                                     std::uint32_t next) noexcept {
    return (std::uint64_t{next} << 17U) | (std::uint64_t{lo} << 9U) | (std::uint64_t{hi} << 1U) | (foldcase ? 1U : 0U);
}

} // namespace

class Compiler {
public:
    Compiler(Ast &ast, const Options &options, bool reversed);

    [[nodiscard]] std::expected<Program, Error> compile(Node *root);
    [[nodiscard]] std::expected<Program, Error> compile_set(Node *root);

private:
    [[nodiscard]] std::expected<Program, Error> finish(int prefix_byte);

    Frag no_match() const noexcept { return {}; }
    Frag plus(Frag a, bool non_greedy);
    Frag star(Frag a, bool non_greedy);
    Frag quest(Frag a, bool non_greedy);
    Frag capture(Frag a, std::uint32_t index);
    Frag cat(Frag a, Frag b);
    Frag alt(Frag a, Frag b);
    Frag match(std::uint32_t id);
    Frag nop();
    Frag byte_range(std::uint32_t lo, std::uint32_t hi, bool foldcase);
    Frag empty_width(std::uint32_t empty);
    Frag literal(Rune r, bool foldcase);
    Frag dot_star();

    // Returns 0 when the instruction budget is spent. Instruction 0 is never
    // handed out here, as the constructor reserves it for Fail.
    std::uint32_t alloc_inst(std::uint32_t count);

    Frag compile_node(const Node *re);
    Frag compile_char_class(const CharClass &cc);

    // Rune range construction. Ranges are built as a tree of shared byte
    // suffixes (forward) or prefixes (reversed) to keep programs small.
    void begin_range();
    void add_rune_range(Rune lo, Rune hi, bool foldcase);
    void add_rune_range_utf8(Rune lo, Rune hi, bool foldcase);
    void add_80_10ffff();
    std::uint32_t uncached_suffix(std::uint8_t lo, std::uint8_t hi, bool foldcase, std::uint32_t next);
    std::uint32_t cached_suffix(std::uint8_t lo, std::uint8_t hi, bool foldcase, std::uint32_t next);
    bool is_cached_suffix(std::uint32_t id) const;
    void add_suffix(std::uint32_t id);
    std::uint32_t add_suffix_recursive(std::uint32_t root, std::uint32_t id);
    Frag find_byte_range(std::uint32_t root, std::uint32_t id) const;
    bool byte_range_equal(std::uint32_t id1, std::uint32_t id2) const noexcept;

    bool is_anchor_start(Node *&re, int depth);
    bool is_anchor_end(Node *&re, int depth);

    Ast &ast_;
    Program program_{};
    std::vector<Inst> inst_{};
    Encoding encoding_;
    bool reversed_;
    bool failed_ = false;
    std::uint32_t max_inst_;
    std::uint64_t visits_ = 0;
    std::uint64_t max_visits_;
    std::unordered_map<std::uint64_t, std::uint32_t> rune_cache_{};
    Frag rune_range_{};
};

Compiler::Compiler(Ast &ast, const Options &options, bool reversed) :
    ast_(ast), encoding_(options.encoding), reversed_(reversed),
    max_inst_(options.max_insts > Inst::kMaxInst ? Inst::kMaxInst : options.max_insts),
    max_visits_(std::uint64_t{2} * max_inst_) {
    inst_.emplace_back();
    inst_[0].init_fail();
}

std::uint32_t Compiler::alloc_inst(std::uint32_t count) {
    if (failed_ || inst_.size() + count > max_inst_) {
        failed_ = true;
        return 0;
    }
    const auto id = static_cast<std::uint32_t>(inst_.size());
    inst_.resize(inst_.size() + count);
    return id;
}

// Returns a fragment for a followed by b. A leading Nop whose out is unset is
// dropped, which keeps the empty regexp from costing an instruction.
Frag Compiler::cat(Frag a, Frag b) {
    if (is_no_match(a) || is_no_match(b)) {
        return no_match();
    }

    const Inst &begin = inst_[a.begin];
    if (begin.opcode() == InstOp::Nop && a.end.head == (a.begin << 1U) && begin.out() == 0) {
        // In a reversed program a Nop still needs b to run first, but a Nop does
        // nothing, so the two orders agree.
        patch(inst_, a.end, b.begin);
        return b;
    }

    // To run backward over the string, reverse all concatenations.
    if (reversed_) {
        patch(inst_, b.end, a.begin);
        return Frag{b.begin, a.end, b.nullable && a.nullable};
    }

    patch(inst_, a.end, b.begin);
    return Frag{a.begin, b.end, a.nullable && b.nullable};
}

Frag Compiler::alt(Frag a, Frag b) {
    // Special case for convenience in loops.
    if (is_no_match(a)) {
        return b;
    }
    if (is_no_match(b)) {
        return a;
    }
    const std::uint32_t id = alloc_inst(1);
    if (id == 0) {
        return no_match();
    }
    inst_[id].init_alt(a.begin, b.begin);
    return Frag{id, append(inst_, a.end, b.end), a.nullable || b.nullable};
}

// When the loop is non-greedy the out arrow is the exit, so that the Alt tries
// leaving first; when greedy, the exit is out1.
Frag Compiler::plus(Frag a, bool non_greedy) {
    const std::uint32_t id = alloc_inst(1);
    if (id == 0) {
        return no_match();
    }
    PatchList list;
    if (non_greedy) {
        inst_[id].init_alt(0, a.begin);
        list = make_patch_list(id << 1U);
    } else {
        inst_[id].init_alt(a.begin, 0);
        list = make_patch_list((id << 1U) | 1U);
    }
    patch(inst_, a.end, id);
    return Frag{a.begin, list, a.nullable};
}

Frag Compiler::star(Frag a, bool non_greedy) {
    // When the subexpression is nullable, one Alt isn't enough to guarantee
    // correct priority ordering within the transitive closure. The simplest fix
    // is to make it a plus, whose loop back is the only way around, and wrap
    // that in a quest. (?:a*)* would otherwise prefer the empty path.
    if (a.nullable) {
        return quest(plus(a, non_greedy), non_greedy);
    }

    const std::uint32_t id = alloc_inst(1);
    if (id == 0) {
        return no_match();
    }
    PatchList list;
    if (non_greedy) {
        inst_[id].init_alt(0, a.begin);
        list = make_patch_list(id << 1U);
    } else {
        inst_[id].init_alt(a.begin, 0);
        list = make_patch_list((id << 1U) | 1U);
    }
    patch(inst_, a.end, id);
    return Frag{id, list, true};
}

Frag Compiler::quest(Frag a, bool non_greedy) {
    if (is_no_match(a)) {
        return nop();
    }
    const std::uint32_t id = alloc_inst(1);
    if (id == 0) {
        return no_match();
    }
    PatchList list;
    if (non_greedy) {
        inst_[id].init_alt(0, a.begin);
        list = make_patch_list(id << 1U);
    } else {
        inst_[id].init_alt(a.begin, 0);
        list = make_patch_list((id << 1U) | 1U);
    }
    return Frag{id, append(inst_, list, a.end), true};
}

Frag Compiler::byte_range(std::uint32_t lo, std::uint32_t hi, bool foldcase) {
    const std::uint32_t id = alloc_inst(1);
    if (id == 0) {
        return no_match();
    }
    inst_[id].init_byte_range(lo, hi, foldcase, 0);
    return Frag{id, make_patch_list(id << 1U), false};
}

Frag Compiler::nop() {
    const std::uint32_t id = alloc_inst(1);
    if (id == 0) {
        return no_match();
    }
    inst_[id].init_nop(0);
    return Frag{id, make_patch_list(id << 1U), true};
}

Frag Compiler::match(std::uint32_t match_id) {
    const std::uint32_t id = alloc_inst(1);
    if (id == 0) {
        return no_match();
    }
    inst_[id].init_match(match_id);
    return Frag{id, PatchList{}, false};
}

Frag Compiler::empty_width(std::uint32_t empty) {
    const std::uint32_t id = alloc_inst(1);
    if (id == 0) {
        return no_match();
    }
    inst_[id].init_empty_width(empty, 0);
    return Frag{id, make_patch_list(id << 1U), true};
}

Frag Compiler::capture(Frag a, std::uint32_t index) {
    if (is_no_match(a)) {
        return no_match();
    }
    const std::uint32_t id = alloc_inst(2);
    if (id == 0) {
        return no_match();
    }
    inst_[id].init_capture(2 * index, a.begin);
    inst_[id + 1].init_capture((2 * index) + 1, 0);
    patch(inst_, a.end, id + 1);
    return Frag{id, make_patch_list((id + 1) << 1U), a.nullable};
}

Frag Compiler::dot_star() { return star(byte_range(0x00, 0xFF, false), true); }

void Compiler::begin_range() {
    rune_cache_.clear();
    rune_range_ = Frag{};
}

std::uint32_t Compiler::uncached_suffix(std::uint8_t lo, std::uint8_t hi, bool foldcase, std::uint32_t next) {
    const Frag frag = byte_range(lo, hi, foldcase);
    if (next != 0) {
        patch(inst_, frag.end, next);
    } else {
        rune_range_.end = append(inst_, rune_range_.end, frag.end);
    }
    return frag.begin;
}

std::uint32_t Compiler::cached_suffix(std::uint8_t lo, std::uint8_t hi, bool foldcase, std::uint32_t next) {
    const std::uint64_t key = rune_cache_key(lo, hi, foldcase, next);
    if (const auto it = rune_cache_.find(key); it != rune_cache_.end()) {
        return it->second;
    }
    const std::uint32_t id = uncached_suffix(lo, hi, foldcase, next);
    rune_cache_[key] = id;
    return id;
}

bool Compiler::is_cached_suffix(std::uint32_t id) const {
    const Inst &ip = inst_[id];
    const std::uint64_t key = rune_cache_key(static_cast<std::uint8_t>(ip.lo()), static_cast<std::uint8_t>(ip.hi()),
                                             ip.foldcase(), ip.out());
    return rune_cache_.contains(key);
}

void Compiler::add_suffix(std::uint32_t id) {
    if (failed_) {
        return;
    }
    if (rune_range_.begin == 0) {
        rune_range_.begin = id;
        return;
    }
    if (encoding_ == Encoding::Utf8) {
        // Build a trie in order to reduce fanout.
        rune_range_.begin = add_suffix_recursive(rune_range_.begin, id);
        return;
    }
    const std::uint32_t alt_id = alloc_inst(1);
    if (alt_id == 0) {
        rune_range_.begin = 0;
        return;
    }
    inst_[alt_id].init_alt(rune_range_.begin, id);
    rune_range_.begin = alt_id;
}

bool Compiler::byte_range_equal(std::uint32_t id1, std::uint32_t id2) const noexcept {
    return inst_[id1].lo() == inst_[id2].lo() && inst_[id1].hi() == inst_[id2].hi() &&
           inst_[id1].foldcase() == inst_[id2].foldcase();
}

// Finds the instruction under `root` with the same byte range as `id`. The
// result names the arrow to it: end.head 0 means root itself.
Frag Compiler::find_byte_range(std::uint32_t root, std::uint32_t id) const {
    if (inst_[root].opcode() == InstOp::ByteRange) {
        return byte_range_equal(root, id) ? Frag{root, PatchList{}, false} : no_match();
    }

    while (inst_[root].opcode() == InstOp::Alt) {
        const std::uint32_t out1 = inst_[root].out1();
        if (byte_range_equal(out1, id)) {
            return Frag{root, make_patch_list((root << 1U) | 1U), false};
        }

        // Forward programs add ranges in sorted order, so only the most recent
        // branch can match. Reversed ones can see any range again.
        if (!reversed_) {
            return no_match();
        }

        const std::uint32_t out = inst_[root].out();
        if (inst_[out].opcode() == InstOp::Alt) {
            root = out;
        } else if (byte_range_equal(out, id)) {
            return Frag{root, make_patch_list(root << 1U), false};
        } else {
            return no_match();
        }
    }

    FIBER_PANIC("regex: byte range trie is malformed");
}

std::uint32_t Compiler::add_suffix_recursive(std::uint32_t root, std::uint32_t id) {
    const Frag found = find_byte_range(root, id);
    if (is_no_match(found)) {
        const std::uint32_t alt_id = alloc_inst(1);
        if (alt_id == 0) {
            return 0;
        }
        inst_[alt_id].init_alt(root, id);
        return alt_id;
    }

    std::uint32_t br = root;
    if (found.end.head != 0) {
        br = (found.end.head & 1U) != 0 ? inst_[found.begin].out1() : inst_[found.begin].out();
    }

    if (is_cached_suffix(br)) {
        // We can't fiddle with cached suffixes, so make a clone of the head.
        const std::uint32_t byterange = alloc_inst(1);
        if (byterange == 0) {
            return 0;
        }
        inst_[byterange].init_byte_range(inst_[br].lo(), inst_[br].hi(), inst_[br].foldcase(), inst_[br].out());

        // Ensure that the parent points to the clone, not to the original.
        // Note that this could leave the head unreachable except via the cache.
        br = byterange;
        if (found.end.head == 0) {
            root = br;
        } else if ((found.end.head & 1U) != 0) {
            inst_[found.begin].set_out1(br);
        } else {
            inst_[found.begin].set_out(br);
        }
    }

    std::uint32_t out = inst_[id].out();
    if (!is_cached_suffix(id)) {
        // The head should be the instruction most recently allocated, so free it
        // instead of wasting it.
        FIBER_ASSERT(id + 1 == inst_.size());
        inst_.pop_back();
    }

    out = add_suffix_recursive(inst_[br].out(), out);
    if (out == 0) {
        return 0;
    }
    inst_[br].set_out(out);
    return root;
}

// 80-10FFFF occurs often enough (for . and [^a-z]) to be worth a shortcut. It
// permits overlong encodings in E0 and F0 sequences and code points over 10FFFF
// in F4 sequences, which makes the bytecode much smaller. Those sequences only
// ever arise from malformed input.
void Compiler::add_80_10ffff() {
    std::uint32_t id = 0;
    if (reversed_) {
        // Prefix factoring is handled by the trie logic.
        id = uncached_suffix(0xC2, 0xDF, false, 0);
        id = uncached_suffix(0x80, 0xBF, false, id);
        add_suffix(id);

        id = uncached_suffix(0xE0, 0xEF, false, 0);
        id = uncached_suffix(0x80, 0xBF, false, id);
        id = uncached_suffix(0x80, 0xBF, false, id);
        add_suffix(id);

        id = uncached_suffix(0xF0, 0xF4, false, 0);
        id = uncached_suffix(0x80, 0xBF, false, id);
        id = uncached_suffix(0x80, 0xBF, false, id);
        id = uncached_suffix(0x80, 0xBF, false, id);
        add_suffix(id);
    } else {
        // Suffix factoring matters here and has to be done by hand.
        const std::uint32_t cont1 = uncached_suffix(0x80, 0xBF, false, 0);
        id = uncached_suffix(0xC2, 0xDF, false, cont1);
        add_suffix(id);

        const std::uint32_t cont2 = uncached_suffix(0x80, 0xBF, false, cont1);
        id = uncached_suffix(0xE0, 0xEF, false, cont2);
        add_suffix(id);

        const std::uint32_t cont3 = uncached_suffix(0x80, 0xBF, false, cont2);
        id = uncached_suffix(0xF0, 0xF4, false, cont3);
        add_suffix(id);
    }
}

void Compiler::add_rune_range_utf8(Rune lo, Rune hi, bool foldcase) {
    if (lo > hi) {
        return;
    }

    if (lo == 0x80 && hi == kRuneMax) {
        add_80_10ffff();
        return;
    }

    // Split the range into same-length sized ranges.
    for (std::size_t i = 1; i < kUtfMax; ++i) {
        const Rune max = max_rune(i);
        if (lo <= max && max < hi) {
            add_rune_range_utf8(lo, max, foldcase);
            add_rune_range_utf8(max + 1, hi, foldcase);
            return;
        }
    }

    // The ASCII range is always a special case.
    if (hi < kRuneSelf) {
        add_suffix(uncached_suffix(static_cast<std::uint8_t>(lo), static_cast<std::uint8_t>(hi), foldcase, 0));
        return;
    }

    // Split the range into sections that agree on leading bytes.
    for (std::size_t i = 1; i < kUtfMax; ++i) {
        const auto m = static_cast<Rune>((std::uint32_t{1} << (6 * i)) - 1U); // last i bytes of a sequence
        if ((lo & ~m) != (hi & ~m)) {
            if ((lo & m) != 0) {
                add_rune_range_utf8(lo, lo | m, foldcase);
                add_rune_range_utf8((lo | m) + 1, hi, foldcase);
                return;
            }
            if ((hi & m) != m) {
                add_rune_range_utf8(lo, (hi & ~m) - 1, foldcase);
                add_rune_range_utf8(hi & ~m, hi, foldcase);
                return;
            }
        }
    }

    // Finally, generate the byte-matching equivalent of lo-hi.
    std::array<std::uint8_t, kUtfMax> ulo{};
    std::array<std::uint8_t, kUtfMax> uhi{};
    const std::size_t n = encode_utf8(lo, ulo.data());
    [[maybe_unused]] const std::size_t m = encode_utf8(hi, uhi.data());
    FIBER_ASSERT(n == m);

    // Caching trades instruction count against clones. A finished suffix cannot
    // be a suffix of anything longer, so its first byte is not worth caching,
    // while its last byte often is. In between, forward mode caches byte ranges
    // and reversed mode caches single bytes, as those are what repeats.
    std::uint32_t id = 0;
    if (reversed_) {
        for (std::size_t i = 0; i < n; ++i) {
            if (i == 0 || (ulo[i] == uhi[i] && i != n - 1)) {
                id = cached_suffix(ulo[i], uhi[i], false, id);
            } else {
                id = uncached_suffix(ulo[i], uhi[i], false, id);
            }
        }
    } else {
        for (std::size_t i = n; i-- > 0;) {
            if (i == n - 1 || (ulo[i] < uhi[i] && i != 0)) {
                id = cached_suffix(ulo[i], uhi[i], false, id);
            } else {
                id = uncached_suffix(ulo[i], uhi[i], false, id);
            }
        }
    }
    add_suffix(id);
}

void Compiler::add_rune_range(Rune lo, Rune hi, bool foldcase) {
    if (encoding_ == Encoding::Utf8) {
        add_rune_range_utf8(lo, hi, foldcase);
        return;
    }
    // In Bytes mode runes *are* bytes.
    if (lo > hi || lo > 0xFF) {
        return;
    }
    hi = std::min<Rune>(hi, 0xFF);
    add_suffix(uncached_suffix(static_cast<std::uint8_t>(lo), static_cast<std::uint8_t>(hi), foldcase, 0));
}

Frag Compiler::literal(Rune r, bool foldcase) {
    if (encoding_ == Encoding::Bytes || r < kRuneSelf) {
        return byte_range(static_cast<std::uint32_t>(r), static_cast<std::uint32_t>(r), foldcase);
    }
    std::array<std::uint8_t, kUtfMax> buf{};
    const std::size_t n = encode_utf8(r, buf.data());
    Frag frag = byte_range(buf[0], buf[0], false);
    for (std::size_t i = 1; i < n; ++i) {
        frag = cat(frag, byte_range(buf[i], buf[i], false));
    }
    return frag;
}

Frag Compiler::compile_char_class(const CharClass &cc) {
    // ASCII case-folding optimization: if the class treats A-Z the same as a-z,
    // discard ranges wholly inside A-Z and mark the others as folding. This cuts
    // (?i)abc from 3 instructions per letter to 1.
    const bool fold_ascii = cc.folds_ascii();

    // A character class is just a big OR of the different ranges in it.
    begin_range();
    for (const RuneRange &range: cc.ranges()) {
        if (fold_ascii && range.lo >= 'A' && range.hi <= 'Z') {
            continue;
        }
        // If the range contains all of A-Za-z or none of it, folding is moot.
        const bool moot = (range.lo <= 'A' && 'z' <= range.hi) || range.hi < 'A' || 'z' < range.lo ||
                          ('Z' < range.lo && range.hi < 'a');
        add_rune_range(range.lo, range.hi, fold_ascii && !moot);
    }
    return rune_range_;
}

Frag Compiler::compile_node(const Node *re) {
    if (failed_) {
        return no_match();
    }
    // The tree is a DAG after simplification, so the walk is exponential in the
    // worst case. Instructions run out long before this in all sane cases.
    if (++visits_ > max_visits_) {
        failed_ = true;
        return no_match();
    }

    switch (re->op) {
        case Op::NoMatch:
            return no_match();
        case Op::EmptyMatch:
            return nop();
        case Op::HaveMatch:
            return match(static_cast<std::uint32_t>(re->cap));

        case Op::Concat: {
            Frag frag = compile_node(re->subs[0]);
            for (std::size_t i = 1; i < re->subs.size(); ++i) {
                frag = cat(frag, compile_node(re->subs[i]));
            }
            return frag;
        }
        case Op::Alternate: {
            Frag frag = compile_node(re->subs[0]);
            for (std::size_t i = 1; i < re->subs.size(); ++i) {
                frag = alt(frag, compile_node(re->subs[i]));
            }
            return frag;
        }

        case Op::Star:
            return star(compile_node(re->subs[0]), re->non_greedy());
        case Op::Plus:
            return plus(compile_node(re->subs[0]), re->non_greedy());
        case Op::Quest:
            return quest(compile_node(re->subs[0]), re->non_greedy());

        case Op::Literal:
            return literal(re->rune, re->fold_case());
        case Op::LiteralString: {
            if (re->runes.empty()) {
                return nop();
            }
            Frag frag = literal(re->runes[0], re->fold_case());
            for (std::size_t i = 1; i < re->runes.size(); ++i) {
                frag = cat(frag, literal(re->runes[i], re->fold_case()));
            }
            return frag;
        }

        case Op::AnyChar:
            begin_range();
            add_rune_range(0, kRuneMax, false);
            return rune_range_;
        case Op::CharClass:
            return compile_char_class(re->cc);

        case Op::Capture:
            return capture(compile_node(re->subs[0]), static_cast<std::uint32_t>(re->cap));

        case Op::BeginLine:
            return empty_width(reversed_ ? kEmptyEndLine : kEmptyBeginLine);
        case Op::EndLine:
            return empty_width(reversed_ ? kEmptyBeginLine : kEmptyEndLine);
        case Op::BeginText:
            return empty_width(reversed_ ? kEmptyEndText : kEmptyBeginText);
        case Op::EndText:
            return empty_width(reversed_ ? kEmptyBeginText : kEmptyEndText);
        case Op::WordBoundary:
            return empty_width(kEmptyWordBoundary);
        case Op::NoWordBoundary:
            return empty_width(kEmptyNonWordBoundary);

        case Op::Repeat:
        case Op::LeftParen:
        case Op::VerticalBar:
            break;
    }
    FIBER_PANIC("regex: unexpected node in compiler");
}

// Is this regexp required to start at the beginning of the text? Only
// approximate: (\Aa|\Ab) is missed but (\A(a|b)) is found. Rewrites `re` without
// the anchor, building new nodes as parts of the tree may be shared.
bool Compiler::is_anchor_start(Node *&re, int depth) {
    if (depth >= 4) {
        return false;
    }
    switch (re->op) {
        case Op::Concat: {
            if (re->subs.empty()) {
                return false;
            }
            Node *sub = re->subs[0];
            if (!is_anchor_start(sub, depth + 1)) {
                return false;
            }
            std::vector<Node *> subs = re->subs;
            subs[0] = sub;
            re = ast_.make_concat(subs.data(), subs.size(), re->flags);
            return true;
        }
        case Op::Capture: {
            Node *sub = re->subs[0];
            if (!is_anchor_start(sub, depth + 1)) {
                return false;
            }
            Node *capture_node = ast_.make(Op::Capture, re->flags);
            capture_node->cap = re->cap;
            capture_node->subs.push_back(sub);
            re = capture_node;
            return true;
        }
        case Op::BeginText:
            re = ast_.make(Op::EmptyMatch, re->flags);
            return true;
        default:
            return false;
    }
}

bool Compiler::is_anchor_end(Node *&re, int depth) {
    if (depth >= 4) {
        return false;
    }
    switch (re->op) {
        case Op::Concat: {
            if (re->subs.empty()) {
                return false;
            }
            Node *sub = re->subs.back();
            if (!is_anchor_end(sub, depth + 1)) {
                return false;
            }
            std::vector<Node *> subs = re->subs;
            subs.back() = sub;
            re = ast_.make_concat(subs.data(), subs.size(), re->flags);
            return true;
        }
        case Op::Capture: {
            Node *sub = re->subs[0];
            if (!is_anchor_end(sub, depth + 1)) {
                return false;
            }
            Node *capture_node = ast_.make(Op::Capture, re->flags);
            capture_node->cap = re->cap;
            capture_node->subs.push_back(sub);
            re = capture_node;
            return true;
        }
        case Op::EndText:
            re = ast_.make(Op::EmptyMatch, re->flags);
            return true;
        default:
            return false;
    }
}

namespace {

// First byte of the literal `re` must start with, or -1. Sees through capturing
// groups but not much else. Case folding rules a single byte out.
int required_prefix_byte(const Node *re, Encoding encoding) noexcept {
    const Node *first = re->op == Op::Concat && !re->subs.empty() ? re->subs[0] : re;
    while (first->op == Op::Capture) {
        first = first->subs[0];
        if (first->op == Op::Concat && !first->subs.empty()) {
            first = first->subs[0];
        }
    }
    if ((first->op != Op::Literal && first->op != Op::LiteralString) || first->fold_case()) {
        return -1;
    }
    const Rune r = first->op == Op::Literal ? first->rune : first->runes[0];
    if (encoding == Encoding::Bytes || r < kRuneSelf) {
        return r;
    }
    std::array<std::uint8_t, kUtfMax> buf{};
    encode_utf8(r, buf.data());
    return buf[0];
}

} // namespace

std::expected<Program, Error> Compiler::compile(Node *root) {
    const bool reversed = reversed_;
    const int prefix_byte = reversed ? -1 : required_prefix_byte(root, encoding_);

    // Record whether the program is anchored, removing the anchors. They get in
    // the way of other optimizations.
    const bool anchor_start = is_anchor_start(root, 0);
    const bool anchor_end = is_anchor_end(root, 0);

    Frag all = compile_node(root);
    if (failed_) {
        return std::unexpected(Error{.code = ErrorCode::ProgramTooLarge, .offset = 0});
    }

    // Put a Match at the end and record the start. Turn reversal off first, to
    // force the remaining concatenations to behave normally.
    reversed_ = false;
    all = cat(all, match(0));

    program_.reversed_ = reversed;
    program_.anchor_start_ = reversed ? anchor_end : anchor_start;
    program_.anchor_end_ = reversed ? anchor_start : anchor_end;
    program_.start_ = all.begin;
    if (!program_.anchor_start_) {
        // Also create an unanchored version, which starts with a .*? loop.
        all = cat(dot_star(), all);
    }
    program_.start_unanchored_ = all.begin;

    auto result = finish(prefix_byte);
    if (result && !reversed) {
        result->build_one_pass();
    }
    return result;
}

std::expected<Program, Error> Compiler::compile_set(Node *root) {
    Frag all = compile_node(root);
    if (failed_) {
        return std::unexpected(Error{.code = ErrorCode::ProgramTooLarge, .offset = 0});
    }

    // The branches end in their own Match instructions, and the loop in front
    // keeps the set searching at every position.
    program_.anchor_start_ = true;
    program_.anchor_end_ = true;
    all = cat(dot_star(), all);
    program_.start_ = all.begin;
    program_.start_unanchored_ = all.begin;
    return finish(-1);
}

std::expected<Program, Error> Compiler::finish(int prefix_byte) {
    if (failed_) {
        return std::unexpected(Error{.code = ErrorCode::ProgramTooLarge, .offset = 0});
    }

    // No possible matches: keep the Fail instruction only.
    if (program_.start_ == 0 && program_.start_unanchored_ == 0) {
        inst_.resize(1);
    }
    program_.prefix_byte_ = prefix_byte;
    program_.inst_ = std::move(inst_);
    program_.optimize();
    program_.flatten();
    program_.compute_byte_map();
    program_.id_ = next_program_id();
    return std::move(program_);
}

std::expected<Program, Error> compile_program(Ast &ast, Node *root, const Options &options, bool reversed) {
    return Compiler(ast, options, reversed).compile(root);
}

std::expected<Program, Error> compile_set(Ast &ast, Node *root, const Options &options) {
    return Compiler(ast, options, false).compile_set(root);
}

} // namespace fiber::regex::detail
