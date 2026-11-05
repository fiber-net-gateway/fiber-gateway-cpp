// Ported from RE2's prog.cc (see UPSTREAM.md).
#include "regex/Program.h"

#include <algorithm>
#include <utility>

#include <fiber/common/Assert.h>

#include "regex/Sparse.h"

namespace fiber::regex::detail {

bool Inst::greedy(const Program &program) const noexcept {
    const Inst &first = program.inst(out());
    return first.opcode() == InstOp::ByteRange ||
           (first.opcode() == InstOp::Nop && program.inst(first.out()).opcode() == InstOp::ByteRange);
}

std::uint32_t Program::empty_flags(std::string_view text, std::size_t pos) noexcept {
    std::uint32_t flags = 0;

    if (pos == 0) {
        flags |= kEmptyBeginText | kEmptyBeginLine;
    } else if (text[pos - 1] == '\n') {
        flags |= kEmptyBeginLine;
    }

    if (pos == text.size()) {
        flags |= kEmptyEndText | kEmptyEndLine;
    } else if (text[pos] == '\n') {
        flags |= kEmptyEndLine;
    }

    const auto word_at = [text](std::size_t i) noexcept { return is_word_char(static_cast<std::uint8_t>(text[i])); };
    const bool before = pos != 0 && word_at(pos - 1);
    const bool after = pos != text.size() && word_at(pos);
    flags |= before != after ? kEmptyWordBoundary : kEmptyNonWordBoundary;

    return flags;
}

namespace {

void add_to_queue(SparseSet &queue, std::uint32_t id) noexcept {
    if (id != 0) {
        queue.insert(id);
    }
}

// Whether ip reaches a match without consuming input or asserting anything.
bool is_match(const std::vector<Inst> &inst, const Inst *ip) noexcept {
    for (;;) {
        switch (ip->opcode()) {
            case InstOp::Capture:
            case InstOp::Nop:
                ip = &inst[ip->out()];
                break;
            case InstOp::Match:
                return true;
            default:
                return false;
        }
    }
}

constexpr std::size_t kMaxBitStateProgram = 512;
constexpr std::size_t kMaxBitStateBitmapBits = std::size_t{256} * 1024U;

// Computes the byte classes of a program by repeatedly splitting and recoloring
// byte ranges. The colors end up naming equivalence classes.
class ByteMapBuilder {
public:
    ByteMapBuilder() noexcept {
        splits_.set(255);
        colors_[255] = 256;
    }

    void mark(std::uint32_t lo, std::uint32_t hi) {
        // Ignore any [00-FF] range: it does not split anything.
        if (lo == 0 && hi == 255) {
            return;
        }
        ranges_.emplace_back(lo, hi);
    }

    void merge() {
        for (const auto &[lo, hi]: ranges_) {
            // Split at lo-1 and hi so that the range is made of whole segments.
            if (lo > 0 && !splits_.test(lo - 1)) {
                splits_.set(lo - 1);
                colors_[lo - 1] = colors_[splits_.find_next_set_bit(lo)];
            }
            if (!splits_.test(hi)) {
                splits_.set(hi);
                colors_[hi] = colors_[splits_.find_next_set_bit(hi + 1)];
            }

            // Recolor the segments that make up the range.
            std::uint32_t c = lo;
            while (c < 256) {
                const std::uint32_t next = splits_.find_next_set_bit(c);
                colors_[next] = recolor(colors_[next]);
                if (next == hi) {
                    break;
                }
                c = next + 1;
            }
        }
        colormap_.clear();
        ranges_.clear();
    }

    // Fills `bytemap` and returns the number of classes.
    std::uint32_t build(std::array<std::uint8_t, 256> &bytemap) {
        // Assign byte classes numbered from 0.
        next_color_ = 0;
        std::uint32_t c = 0;
        while (c < 256) {
            const std::uint32_t next = splits_.find_next_set_bit(c);
            const auto color = static_cast<std::uint8_t>(recolor(colors_[next]));
            for (; c <= next; ++c) {
                bytemap[c] = color;
            }
        }
        return next_color_;
    }

private:
    std::uint32_t recolor(std::uint32_t old_color) {
        // Yes, this is a linear search. There can be at most 256 colors and there
        // will typically be far fewer than that. Also, old_color may be either
        // key or value: a color already remapped this round stays as it is.
        const auto it = std::find_if(colormap_.begin(), colormap_.end(), [old_color](const auto &entry) {
            return entry.first == old_color || entry.second == old_color;
        });
        if (it != colormap_.end()) {
            return it->second;
        }
        const std::uint32_t new_color = next_color_++;
        colormap_.emplace_back(old_color, new_color);
        return new_color;
    }

    Bitmap256 splits_{};
    std::array<std::uint32_t, 256> colors_{};
    std::uint32_t next_color_ = 257;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> colormap_{};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges_{};
};

// Scratch state shared by the three passes of flatten().
struct FlattenState {
    SparseArray<std::uint32_t> rootmap{};
    SparseArray<std::uint32_t> predmap{};
    std::vector<std::vector<std::uint32_t>> predvec{};
    SparseSet reachable{};
    std::vector<std::uint32_t> stack{};

    void add_root(std::uint32_t id) noexcept {
        if (!rootmap.has_index(id)) {
            rootmap.set_new(id, rootmap.size());
        }
    }

    void add_predecessor(std::uint32_t out, std::uint32_t id) {
        if (!predmap.has_index(out)) {
            predmap.set_new(out, static_cast<std::uint32_t>(predvec.size()));
            predvec.emplace_back();
        }
        predvec[predmap.get_existing(out)].push_back(id);
    }
};

// Records the predecessors of every Alt branch and marks the successors of
// consuming instructions as list roots.
void mark_successors(const std::vector<Inst> &inst, std::uint32_t start, FlattenState &state) {
    state.reachable.clear();
    state.stack.clear();
    state.stack.push_back(start);
    while (!state.stack.empty()) {
        std::uint32_t id = state.stack.back();
        state.stack.pop_back();
        while (!state.reachable.contains(id)) {
            state.reachable.insert_new(id);
            const Inst &ip = inst[id];
            switch (ip.opcode()) {
                case InstOp::AltMatch:
                case InstOp::Alt:
                    state.add_predecessor(ip.out(), id);
                    state.add_predecessor(ip.out1(), id);
                    state.stack.push_back(ip.out1());
                    id = ip.out();
                    continue;
                case InstOp::ByteRange:
                case InstOp::Capture:
                case InstOp::EmptyWidth:
                    state.add_root(ip.out());
                    id = ip.out();
                    continue;
                case InstOp::Nop:
                    id = ip.out();
                    continue;
                case InstOp::Match:
                case InstOp::Fail:
                    break;
            }
            break;
        }
    }
}

// Marks any instruction that `root` does not dominate as a root as well, so that
// lists never overlap.
void mark_dominator(const std::vector<Inst> &inst, std::uint32_t root, FlattenState &state) {
    state.reachable.clear();
    state.stack.clear();
    state.stack.push_back(root);
    while (!state.stack.empty()) {
        std::uint32_t id = state.stack.back();
        state.stack.pop_back();
        while (!state.reachable.contains(id)) {
            state.reachable.insert_new(id);
            // Stop at any other root: it starts its own list.
            if (id != root && state.rootmap.has_index(id)) {
                break;
            }
            const Inst &ip = inst[id];
            if (ip.opcode() == InstOp::AltMatch || ip.opcode() == InstOp::Alt) {
                state.stack.push_back(ip.out1());
                id = ip.out();
                continue;
            }
            if (ip.opcode() == InstOp::Nop) {
                id = ip.out();
                continue;
            }
            break;
        }
    }

    for (std::uint32_t i = 0; i < state.reachable.size(); ++i) {
        const std::uint32_t id = state.reachable.at(i);
        if (!state.predmap.has_index(id)) {
            continue;
        }
        for (const std::uint32_t pred: state.predvec[state.predmap.get_existing(id)]) {
            // An instruction reached from outside this list must be a root.
            if (!state.reachable.contains(pred)) {
                state.add_root(id);
            }
        }
    }
}

// Appends the list rooted at `root` to `flat`.
void emit_list(const std::vector<Inst> &inst, std::uint32_t root, std::vector<Inst> &flat, FlattenState &state) {
    state.reachable.clear();
    state.stack.clear();
    state.stack.push_back(root);
    while (!state.stack.empty()) {
        std::uint32_t id = state.stack.back();
        state.stack.pop_back();
        while (!state.reachable.contains(id)) {
            state.reachable.insert_new(id);

            if (id != root && state.rootmap.has_index(id)) {
                // Another list begins here: jump to it.
                flat.emplace_back();
                flat.back().set_opcode(InstOp::Nop);
                flat.back().set_out(state.rootmap.get_existing(id));
                break;
            }

            const Inst &ip = inst[id];
            switch (ip.opcode()) {
                case InstOp::AltMatch:
                    // Must be immediately followed by the two branches it
                    // represents, in the order the Alt visits them.
                    flat.emplace_back();
                    flat.back().set_opcode(InstOp::AltMatch);
                    flat.back().set_out(static_cast<std::uint32_t>(flat.size()));
                    flat.back().set_out1(static_cast<std::uint32_t>(flat.size()) + 1U);
                    [[fallthrough]];
                case InstOp::Alt:
                    state.stack.push_back(ip.out1());
                    id = ip.out();
                    continue;
                case InstOp::ByteRange:
                case InstOp::Capture:
                case InstOp::EmptyWidth:
                    flat.push_back(ip);
                    flat.back().set_out(state.rootmap.get_existing(ip.out()));
                    break;
                case InstOp::Nop:
                    id = ip.out();
                    continue;
                case InstOp::Match:
                case InstOp::Fail:
                    flat.push_back(ip);
                    break;
            }
            break;
        }
    }
}

// For each ByteRange in flat[begin, end) computes the distance to the next
// instruction that could also match, so an engine that found a match can skip
// the rest of the list. Runs backward, colouring byte values with the id of the
// nearest later ByteRange that accepts them.
void compute_hints(std::vector<Inst> &flat, std::uint32_t begin, std::uint32_t end) {
    Bitmap256 splits;
    std::array<std::uint32_t, 256> colors{};

    bool dirty = false;
    for (std::uint32_t id = end + 1; id-- > begin;) {
        if (id == end || flat[id].opcode() != InstOp::ByteRange) {
            if (dirty) {
                dirty = false;
                splits.clear();
            }
            splits.set(255);
            colors[255] = id;
            // At this point, the first ByteRange seen will not have a hint.
            continue;
        }
        dirty = true;

        std::uint32_t first = end;
        const auto recolor = [&](std::uint32_t lo, std::uint32_t hi) {
            // Split at lo-1 and hi so that the span is made of whole segments.
            if (lo > 0 && !splits.test(lo - 1)) {
                splits.set(lo - 1);
                colors[lo - 1] = colors[splits.find_next_set_bit(lo)];
            }
            if (!splits.test(hi)) {
                splits.set(hi);
                colors[hi] = colors[splits.find_next_set_bit(hi + 1)];
            }
            std::uint32_t c = lo;
            while (c < 256) {
                const std::uint32_t next = splits.find_next_set_bit(c);
                first = std::min(first, colors[next]);
                colors[next] = id;
                if (next == hi) {
                    break;
                }
                c = next + 1;
            }
        };

        Inst &ip = flat[id];
        const std::uint32_t lo = ip.lo();
        const std::uint32_t hi = ip.hi();
        recolor(lo, hi);
        if (ip.foldcase() && lo <= 'z' && hi >= 'a') {
            const std::uint32_t fold_lo = std::max<std::uint32_t>(lo, 'a') + 'A' - 'a';
            const std::uint32_t fold_hi = std::min<std::uint32_t>(hi, 'z') + 'A' - 'a';
            if (fold_lo <= fold_hi) {
                recolor(fold_lo, fold_hi);
            }
        }

        if (first != end) {
            ip.set_hint(std::min<std::uint32_t>(first - id, 32767U));
        }
    }
}

} // namespace

void Program::optimize() {
    SparseSet queue;
    queue.ensure_capacity(size());

    // Eliminate Nops. Most come from \z and (?:) and the like.
    add_to_queue(queue, start_);
    for (std::uint32_t i = 0; i < queue.size(); ++i) {
        const std::uint32_t id = queue.at(i);
        Inst &ip = inst_[id];
        std::uint32_t j = ip.out();
        while (j != 0 && inst_[j].opcode() == InstOp::Nop) {
            j = inst_[j].out();
        }
        ip.set_out(j);
        add_to_queue(queue, ip.out());

        if (ip.opcode() == InstOp::Alt) {
            j = ip.out1();
            while (j != 0 && inst_[j].opcode() == InstOp::Nop) {
                j = inst_[j].out();
            }
            ip.set_out1(j);
            add_to_queue(queue, ip.out1());
        }
    }

    // Insert AltMatch instructions: a tail .* or .*? followed by a match.
    queue.clear();
    add_to_queue(queue, start_);
    for (std::uint32_t i = 0; i < queue.size(); ++i) {
        const std::uint32_t id = queue.at(i);
        Inst &ip = inst_[id];
        add_to_queue(queue, ip.out());
        if (ip.opcode() != InstOp::Alt) {
            continue;
        }
        add_to_queue(queue, ip.out1());

        const Inst &j = inst_[ip.out()];
        const Inst &k = inst_[ip.out1()];
        const auto loops_over_any_byte = [id](const Inst &x) {
            return x.opcode() == InstOp::ByteRange && x.out() == id && x.lo() == 0x00 && x.hi() == 0xFF;
        };
        if ((loops_over_any_byte(j) && is_match(inst_, &k)) || (is_match(inst_, &j) && loops_over_any_byte(k))) {
            ip.set_opcode(InstOp::AltMatch);
        }
    }
}

void Program::flatten() {
    const std::uint32_t count = size();
    FlattenState state;
    state.rootmap.ensure_capacity(count);
    state.predmap.ensure_capacity(count);
    state.reachable.ensure_capacity(count);
    state.stack.reserve(count);

    // Instruction 0 and both entry points are always roots.
    state.add_root(0);
    state.add_root(start_unanchored_);
    state.add_root(start_);
    mark_successors(inst_, start_unanchored_, state);

    // Visit roots from highest to lowest, skipping the Fail instruction, so that
    // a root's dominance is settled before the roots that lead to it. The entry
    // points already are roots and need no further marking.
    std::vector<std::uint32_t> sorted;
    sorted.reserve(state.rootmap.size());
    for (std::uint32_t i = 0; i < state.rootmap.size(); ++i) {
        sorted.push_back(state.rootmap.entry_at(i).index);
    }
    std::sort(sorted.begin(), sorted.end());
    for (std::size_t i = sorted.size(); i-- > 1;) {
        if (sorted[i] != start_unanchored_ && sorted[i] != start_) {
            mark_dominator(inst_, sorted[i], state);
        }
    }

    // Emit the lists. rootmap values are list numbers; flatmap turns them into
    // instruction ids in the flattened program.
    std::vector<std::uint32_t> flatmap(state.rootmap.size());
    std::vector<Inst> flat;
    flat.reserve(count);
    for (std::uint32_t i = 0; i < state.rootmap.size(); ++i) {
        const SparseEntry<std::uint32_t> entry = state.rootmap.entry_at(i);
        flatmap[entry.value] = static_cast<std::uint32_t>(flat.size());
        emit_list(inst_, entry.index, flat, state);
        flat.back().set_last();
        compute_hints(flat, flatmap[entry.value], static_cast<std::uint32_t>(flat.size()));
    }

    list_count_ = static_cast<std::uint32_t>(flatmap.size());
    inst_count_.fill(0);

    // Remap the outs from list numbers to instruction ids.
    for (Inst &ip: flat) {
        if (ip.opcode() != InstOp::AltMatch) { // emit_list already placed these
            ip.set_out(flatmap[ip.out()]);
        }
        ++inst_count_[static_cast<std::size_t>(ip.opcode())];
    }

    // The entry points were added right after the Fail list, in this order.
    if (start_unanchored_ == 0) {
        FIBER_ASSERT(start_ == 0);
    } else if (start_unanchored_ == start_) {
        start_unanchored_ = flatmap[1];
        start_ = flatmap[1];
    } else {
        start_unanchored_ = flatmap[1];
        start_ = flatmap[2];
    }

    inst_ = std::move(flat);

    // Indexed by instruction, so that BitState can find a list by its head.
    if (inst_.size() <= kMaxBitStateProgram) {
        list_heads_.assign(inst_.size(), 0xFFFFU);
        for (std::uint32_t i = 0; i < list_count_; ++i) {
            list_heads_[flatmap[i]] = static_cast<std::uint16_t>(i);
        }
    }
    FIBER_ASSERT(list_count_ > 0); // the Fail list is always there
    bit_state_text_max_size_ = (kMaxBitStateBitmapBits / list_count_) - 1;
}

void Program::compute_byte_map() {
    ByteMapBuilder builder;

    bool marked_line_boundaries = false;
    bool marked_word_boundaries = false;

    for (std::uint32_t id = 0; id < size(); ++id) {
        const Inst &ip = inst_[id];
        if (ip.opcode() == InstOp::ByteRange) {
            const std::uint32_t lo = ip.lo();
            const std::uint32_t hi = ip.hi();
            builder.mark(lo, hi);
            if (ip.foldcase() && lo <= 'z' && hi >= 'a') {
                const std::uint32_t fold_lo = std::max<std::uint32_t>(lo, 'a') + 'A' - 'a';
                const std::uint32_t fold_hi = std::min<std::uint32_t>(hi, 'z') + 'A' - 'a';
                if (fold_lo <= fold_hi) {
                    builder.mark(fold_lo, fold_hi);
                }
            }
            // If this instruction is not the last in its list, the next one is a
            // ByteRange too, and they share an out, then defer the merge: they
            // cannot be told apart, so they recolor together.
            if (!ip.last() && inst_[id + 1].opcode() == InstOp::ByteRange && ip.out() == inst_[id + 1].out()) {
                continue;
            }
            builder.merge();
        } else if (ip.opcode() == InstOp::EmptyWidth) {
            if ((ip.empty() & (kEmptyBeginLine | kEmptyEndLine)) != 0 && !marked_line_boundaries) {
                builder.mark('\n', '\n');
                builder.merge();
                marked_line_boundaries = true;
            }
            if ((ip.empty() & (kEmptyWordBoundary | kEmptyNonWordBoundary)) != 0 && !marked_word_boundaries) {
                // Two separate rounds, so that word and non-word characters get
                // different colors.
                for (const bool is_word: {true, false}) {
                    std::uint32_t j = 0;
                    for (std::uint32_t i = 0; i < 256; i = j) {
                        const bool word = is_word_char(static_cast<std::uint8_t>(i));
                        for (j = i + 1; j < 256 && is_word_char(static_cast<std::uint8_t>(j)) == word; ++j) {
                        }
                        if (word == is_word) {
                            builder.mark(i, j - 1);
                        }
                    }
                    builder.merge();
                }
                marked_word_boundaries = true;
            }
        }
    }

    bytemap_range_ = builder.build(bytemap_);
}

} // namespace fiber::regex::detail
