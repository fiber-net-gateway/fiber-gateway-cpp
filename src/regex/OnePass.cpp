// Ported from RE2's onepass.cc (see UPSTREAM.md).
//
// A program is one-pass if, whatever the input, only one thread can make it past
// a ByteRange at each byte. /x*yx*/ is one-pass: read x's until a y, then keep
// reading x's. /x*x/ is not: it is unclear whether an x extends x* or is the final
// x. Such a program has an equivalent DFA with at most as many states as the NFA,
// where each state records, for each byte class, the next state and the conditions
// to get there: empty-width flags that must hold and captures to perform.
#include "regex/OnePass.h"

#include <algorithm>
#include <array>
#include <vector>

#include <fiber/common/Assert.h>

#include "regex/Sparse.h"

namespace fiber::regex::detail {

namespace {

// A state is matchcond followed by one action per byte class, and an action holds
// the next state's index in its top 16 bits. Below that:
//   bits 0-5: the empty-width flags from Program.h
//   bit 6:    kMatchWins, the match takes priority over moving to the next state
//   bits 7-:  capture registers to set, starting at 2 as the loop handles $0
// No position can satisfy both word-boundary flags, so that pair marks "impossible".
constexpr std::uint32_t kIndexShift = 16;
constexpr std::uint32_t kEmptyShift = 6;
constexpr std::uint32_t kEmptyAllFlags = (1U << kEmptyShift) - 1U;
constexpr std::uint32_t kRealCapShift = kEmptyShift + 1;
constexpr std::uint32_t kRealMaxCap = (kIndexShift - kRealCapShift) / 2 * 2;
constexpr std::uint32_t kCapShift = kRealCapShift - 2; // skips over cap[0], cap[1]
constexpr std::uint32_t kMaxCap = kRealMaxCap + 2;
constexpr std::uint32_t kMatchWins = 1U << kEmptyShift;
constexpr std::uint32_t kCapMask = ((1U << kRealMaxCap) - 1U) << kRealCapShift;
constexpr std::uint32_t kImpossible = kEmptyWordBoundary | kEmptyNonWordBoundary;

static_assert(kMaxCap == kMaxOnePassCapture * 2);

// Conditions cap memory use: nodes are not worth more than this per program.
constexpr std::size_t kMaxOnePassBytes = std::size_t{1} << 20U;
constexpr std::uint32_t kMaxNodes = 65000; // indexes must fit in 16 bits

constexpr std::size_t kUnset = Span::npos;

[[nodiscard]] bool satisfied(std::uint32_t cond, std::string_view context, std::size_t pos) noexcept {
    return (cond & kEmptyAllFlags & ~Program::empty_flags(context, pos)) == 0;
}

void apply_captures(std::uint32_t cond, std::size_t pos, std::size_t *cap, std::size_t ncap) noexcept {
    for (std::size_t i = 2; i < ncap; ++i) {
        if ((cond & ((1U << kCapShift) << i)) != 0) {
            cap[i] = pos;
        }
    }
}

struct InstCond {
    std::uint32_t id = 0;
    std::uint32_t cond = 0;
};

// Adds id to the queue. Returns false if it was already there. 0 stands for no
// instruction and always succeeds.
bool add_once(SparseSet &queue, std::uint32_t id) noexcept {
    if (id == 0) {
        return true;
    }
    if (queue.contains(id)) {
        return false;
    }
    queue.insert_new(id);
    return true;
}

} // namespace

MatchStatus one_pass_search(const Program &program, const SearchRequest &request, WorkBudget &budget) noexcept {
    const std::string_view context = request.context;
    const std::size_t begin = request.begin;
    const std::size_t end = request.end;

    if (program.anchor_start() && begin != 0) {
        return MatchStatus::NoMatch;
    }
    if (program.anchor_end() && end != context.size()) {
        return MatchStatus::NoMatch;
    }
    if (budget.remaining < end - begin) {
        return MatchStatus::BudgetExhausted;
    }
    budget.remaining -= end - begin;

    const bool full_match = request.full_match || program.anchor_end();
    const bool longest = request.longest;
    const std::size_t nmatch = request.captures.size();
    FIBER_ASSERT(nmatch <= kMaxOnePassCapture);

    // Make sure there is at least cap[1], which tells whether anything matched.
    const std::size_t ncap = 2 * std::max<std::size_t>(nmatch, 1);
    std::array<std::size_t, kMaxCap> cap{};
    std::array<std::size_t, kMaxCap> matchcap{};
    cap.fill(kUnset);
    matchcap.fill(kUnset);

    const std::vector<std::uint32_t> &nodes = program.one_pass_nodes();
    const std::size_t stride = std::size_t{program.bytemap_range()} + 1;
    const std::array<std::uint8_t, 256> &bytemap = program.bytemap();

    // start() is always mapped to the zeroth state.
    const std::uint32_t *state = nodes.data();
    bool alive = true;
    bool matched = false;
    matchcap[0] = begin;
    cap[0] = begin;
    std::uint32_t next_matchcond = state[0];

    std::size_t pos = begin;
    for (; pos < end; ++pos) {
        const std::uint32_t c = bytemap[static_cast<unsigned char>(context[pos])];
        const std::uint32_t matchcond = next_matchcond;
        const std::uint32_t cond = state[1 + c];

        // Determine whether the next state can be reached.
        if ((cond & kEmptyAllFlags) == 0 || satisfied(cond, context, pos)) {
            state = nodes.data() + (std::size_t{cond >> kIndexShift} * stride);
            next_matchcond = state[0];
        } else {
            alive = false;
            next_matchcond = kImpossible;
        }

        // Saving the match registers is expensive, so skip intermediate matches
        // that a full match would ignore, that are impossible, or that are beaten
        // by the certain match at the next byte.
        const bool worth_checking = !full_match && matchcond != kImpossible &&
                                    ((cond & kMatchWins) != 0 || (next_matchcond & kEmptyAllFlags) != 0);
        if (worth_checking && ((matchcond & kEmptyAllFlags) == 0 || satisfied(matchcond, context, pos))) {
            std::copy(cap.begin() + 2, cap.begin() + static_cast<std::ptrdiff_t>(ncap), matchcap.begin() + 2);
            if (nmatch > 1 && (matchcond & kCapMask) != 0) {
                apply_captures(matchcond, pos, matchcap.data(), ncap);
            }
            matchcap[1] = pos;
            matched = true;

            // Keep going for a longer match, unless the match takes priority over
            // the next state for this byte. That is per byte, so it is in cond.
            if (!longest && (cond & kMatchWins) != 0) {
                alive = false;
                break;
            }
        }

        if (!alive) {
            break;
        }
        if ((cond & kCapMask) != 0 && nmatch > 1) {
            apply_captures(cond, pos, cap.data(), ncap);
        }
    }

    // Look for a match at the end of the input.
    if (alive) {
        const std::uint32_t matchcond = state[0];
        if (matchcond != kImpossible && ((matchcond & kEmptyAllFlags) == 0 || satisfied(matchcond, context, pos))) {
            if (nmatch > 1 && (matchcond & kCapMask) != 0) {
                apply_captures(matchcond, pos, cap.data(), ncap);
            }
            std::copy(cap.begin() + 2, cap.begin() + static_cast<std::ptrdiff_t>(ncap), matchcap.begin() + 2);
            matchcap[1] = pos;
            matched = true;
        }
    }

    if (!matched) {
        return MatchStatus::NoMatch;
    }
    for (std::size_t i = 0; i < nmatch; ++i) {
        Span &span = request.captures[i];
        span.begin = matchcap[2 * i];
        span.end = matchcap[(2 * i) + 1];
        if (span.end == kUnset) {
            span.begin = kUnset;
        }
    }
    return MatchStatus::Match;
}

// Decides whether the program is one-pass and, if so, builds the equivalent DFA.
// For any instruction ip, these must hold:
//   (1) for any other instruction nip, at most one input-free path leads from ip
//       to nip;
//   (2) at most one ByteRange reachable from ip matches any particular byte;
//   (3) at most one input-free path leads from ip to a Match.
// This is conservative: it may say no to a program with EmptyWidth instructions
// that really is one-pass.
void Program::build_one_pass() {
    if (start_ == 0) {
        return; // no match
    }

    const std::uint32_t max_nodes = 2 + inst_count(InstOp::ByteRange);
    const std::size_t stride = std::size_t{bytemap_range_} + 1;
    if (max_nodes >= kMaxNodes || std::size_t{max_nodes} * stride * sizeof(std::uint32_t) > kMaxOnePassBytes) {
        return;
    }

    // Flood the graph from the start state, checking that in each reachable state
    // each possible byte leads to a unique next state.
    std::vector<InstCond> stack(std::size_t{inst_count(InstOp::Capture)} + inst_count(InstOp::EmptyWidth) +
                                inst_count(InstOp::Nop) + 1);
    std::vector<std::int32_t> node_by_id(size(), -1);
    std::vector<std::uint32_t> nodes(stride, 0);

    SparseSet to_visit;
    SparseSet work;
    to_visit.ensure_capacity(size());
    work.ensure_capacity(size());
    add_once(to_visit, start_);
    node_by_id[start_] = 0;
    std::uint32_t allocated = 1;

    for (std::uint32_t visit = 0; visit < to_visit.size(); ++visit) {
        const std::uint32_t root = to_visit.at(visit);
        const auto node_index = static_cast<std::uint32_t>(node_by_id[root]);
        const auto node_at = [&nodes, stride](std::uint32_t index) { return nodes.data() + (index * stride); };

        // Default to no action.
        std::fill_n(node_at(node_index), stride, kImpossible);

        work.clear();
        bool matched = false;
        std::size_t depth = 0;
        stack[depth++] = InstCond{.id = root, .cond = 0};

        while (depth > 0) {
            --depth;
            std::uint32_t id = stack[depth].id;
            std::uint32_t cond = stack[depth].cond;

            for (;;) {
                const Inst &ip = inst(id);
                bool advance = false;

                switch (ip.opcode()) {
                    case InstOp::AltMatch:
                        // The AltMatch optimization is ignored here: subtle, and
                        // the list below it says the same thing.
                        if (!add_once(work, id + 1)) {
                            return;
                        }
                        ++id;
                        advance = true;
                        break;

                    case InstOp::ByteRange: {
                        std::int32_t next_index = node_by_id[ip.out()];
                        if (next_index == -1) {
                            if (allocated >= max_nodes) {
                                return;
                            }
                            next_index = static_cast<std::int32_t>(allocated);
                            add_once(to_visit, ip.out());
                            node_by_id[ip.out()] = next_index;
                            ++allocated;
                            nodes.resize(nodes.size() + stride, 0);
                        }

                        const std::uint32_t new_action = (static_cast<std::uint32_t>(next_index) << kIndexShift) |
                                                         cond | (matched ? kMatchWins : 0U);
                        const auto set_action = [&](std::uint32_t lo, std::uint32_t hi) {
                            for (std::uint32_t c = lo; c <= hi; ++c) {
                                const std::uint8_t b = bytemap_[c];
                                // Skip any bytes immediately after c that are also in b.
                                while (c < 255 && bytemap_[c + 1] == b) {
                                    ++c;
                                }
                                std::uint32_t &action = node_at(node_index)[1 + b];
                                if ((action & kImpossible) == kImpossible) {
                                    action = new_action;
                                } else if (action != new_action) {
                                    return false;
                                }
                            }
                            return true;
                        };

                        if (!set_action(ip.lo(), ip.hi())) {
                            return;
                        }
                        // Literals keep the flag even when they are not letters, as
                        // in (?i)\n, so the range may not reach a-z at all.
                        if (ip.foldcase() && ip.lo() <= 'z' && ip.hi() >= 'a') {
                            const std::uint32_t lo = std::max<std::uint32_t>(ip.lo(), 'a') + 'A' - 'a';
                            const std::uint32_t hi = std::min<std::uint32_t>(ip.hi(), 'z') + 'A' - 'a';
                            if (!set_action(lo, hi)) {
                                return;
                            }
                        }

                        if (ip.last()) {
                            break;
                        }
                        if (!add_once(work, id + 1)) {
                            return;
                        }
                        ++id;
                        advance = true;
                        break;
                    }

                    case InstOp::Capture:
                    case InstOp::EmptyWidth:
                    case InstOp::Nop:
                        if (!ip.last()) {
                            if (!add_once(work, id + 1)) {
                                return;
                            }
                            stack[depth++] = InstCond{.id = id + 1, .cond = cond};
                        }

                        if (ip.opcode() == InstOp::Capture && ip.cap() < kMaxCap) {
                            cond |= (1U << kCapShift) << ip.cap();
                        }
                        if (ip.opcode() == InstOp::EmptyWidth) {
                            cond |= ip.empty();
                        }

                        // EmptyWidth only sometimes proceeds, but assuming that it
                        // always does is a safe approximation.
                        if (!add_once(work, ip.out())) {
                            return;
                        }
                        id = ip.out();
                        advance = true;
                        break;

                    case InstOp::Match:
                        if (matched) {
                            return; // (3) is violated
                        }
                        matched = true;
                        node_at(node_index)[0] = cond;

                        if (ip.last()) {
                            break;
                        }
                        if (!add_once(work, id + 1)) {
                            return;
                        }
                        ++id;
                        advance = true;
                        break;

                    case InstOp::Fail:
                        break;

                    case InstOp::Alt:
                        FIBER_PANIC("regex: Alt survived flattening");
                }

                if (!advance) {
                    break;
                }
            }
        }
    }

    one_pass_nodes_ = std::move(nodes);
}

} // namespace fiber::regex::detail
