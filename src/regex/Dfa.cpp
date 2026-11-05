// A lazily built DFA, ported from RE2's dfa.cc (see UPSTREAM.md and section 8 of
// feature/regex.md, which explains how this one differs).
//
// A DFA state is a list of NFA instructions plus flags. Rather than building the
// whole automaton, states are computed the first time they are needed and cached,
// so the common path is one table lookup per input byte.
#include "regex/Dfa.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::regex::detail {

namespace {

constexpr int kByteEndText = 256; // imaginary byte at the end of the text

constexpr std::uint32_t kFlagEmptyMask = 0xFFU; // bits holding the kEmpty flags
constexpr std::uint32_t kFlagMatch = 0x0100U; // this is a matching state
constexpr std::uint32_t kFlagLastWord = 0x0200U; // the last byte was a word character
constexpr std::uint32_t kFlagNeedShift = 16; // needed kEmpty bits are stored here

constexpr std::uint32_t kMark = 0xFFFFFFFFU; // separates thread groups of different priority
constexpr std::uint32_t kMatchSep = 0xFFFFFFFEU; // separates match ids from instructions

// Indexes into the start states. kStartAnchored is added for anchored searches.
constexpr std::size_t kStartBeginText = 0;
constexpr std::size_t kStartBeginLine = 2;
constexpr std::size_t kStartAfterWordChar = 4;
constexpr std::size_t kStartAfterNonWordChar = 6;
constexpr std::size_t kStartAnchored = 1;

// In the state graph a null pointer means "not computed yet". These two mean that
// no match is possible any more, and that everything from here on matches.
DfaState *dead_state() noexcept { return reinterpret_cast<DfaState *>(std::uintptr_t{1}); }
DfaState *full_match_state() noexcept { return reinterpret_cast<DfaState *>(std::uintptr_t{2}); }
bool is_special(const DfaState *state) noexcept { return reinterpret_cast<std::uintptr_t>(state) <= 2; }

// Below this many bytes per state computed since the last reset, the cache is only
// getting in the way: stepping without it is cheaper than refilling it.
constexpr std::size_t kMinBytesPerState = 10;

std::uint64_t next_epoch() noexcept {
    static std::atomic<std::uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

std::uint32_t hash_state(const std::uint32_t *inst, std::uint32_t ninst, std::uint32_t flag) noexcept {
    std::uint32_t hash = 2166136261U ^ flag;
    for (std::uint32_t i = 0; i < ninst; ++i) {
        hash = (hash ^ inst[i]) * 16777619U;
    }
    return hash ^ (hash >> 15U);
}

// A state that may or may not be in the cache.
struct StateView {
    const std::uint32_t *inst = nullptr;
    std::uint32_t ninst = 0;
    std::uint32_t flag = 0;

    [[nodiscard]] bool is_match() const noexcept { return (flag & kFlagMatch) != 0; }
};

[[nodiscard]] StateView view_of(const DfaState *state) noexcept {
    return {.inst = state->inst, .ninst = state->ninst, .flag = state->flag};
}

void collect_matches(const StateView &state, MatchSet *matches) noexcept {
    if (matches == nullptr) {
        return;
    }
    for (std::uint32_t i = state.ninst; i-- > 0;) {
        if (state.inst[i] == kMatchSep) {
            break;
        }
        matches->insert(state.inst[i]);
    }
}

// The moving parts of one pass over some text.
struct Scan {
    const std::uint8_t *p = nullptr; // scanning point
    const std::uint8_t *ep = nullptr; // where to stop
    const std::uint8_t *last_match = nullptr;
    const std::uint8_t *reset_p = nullptr; // p at the last cache reset
    DfaState *start = nullptr;
    DfaState *state = nullptr;
    MatchSet *matches = nullptr;
    bool matched = false;
    bool stopped = false; // no need to look at more input
    bool exhausted = false; // the work budget ran out
};

class DfaRun {
public:
    DfaRun(const Program &program, DfaKind kind, DfaWorkspace &workspace, WorkBudget &budget) :
        program_(program), kind_(kind), ws_(workspace), budget_(budget), cache_(ws_.cache_for(program, kind)),
        next_count_(program.bytemap_range() + 1), q0_(&ws_.q0), q1_(&ws_.q1) {
        const std::uint32_t size = program.size();
        const std::uint32_t marks = kind == DfaKind::LongestMatch ? size : 0;
        ws_.q0.reset(size, marks);
        ws_.q1.reset(size, marks);

        // Each instruction is processed at most once: see add_to_queue().
        const std::size_t stack_size = std::size_t{program.inst_count(InstOp::Capture)} +
                                       program.inst_count(InstOp::EmptyWidth) + program.inst_count(InstOp::Nop) +
                                       marks + 1;
        if (ws_.stack.size() < stack_size) {
            ws_.stack.resize(stack_size);
        }
        const std::size_t buffer_size = std::size_t{size} + marks + program.inst_count(InstOp::Match) + 2;
        for (std::vector<std::uint32_t> &buffer: ws_.buffers) {
            if (buffer.size() < buffer_size) {
                buffer.resize(buffer_size);
            }
        }
    }

    [[nodiscard]] DfaResult search(const DfaRequest &request, bool anchored, bool earliest, bool forward) noexcept;
    [[nodiscard]] MatchStatus feed(DfaCursor &cursor, std::string_view chunk, MatchSet &matches) noexcept;
    [[nodiscard]] MatchStatus finish(DfaCursor &cursor, MatchSet &matches) noexcept;

private:
    enum class Outcome : std::uint8_t { State, Dead, FullMatch };

    [[nodiscard]] bool charge(std::uint64_t cost) noexcept {
        if (budget_.remaining < cost) {
            return false;
        }
        budget_.remaining -= cost;
        return true;
    }

    [[nodiscard]] std::uint32_t byte_class(int c) const noexcept {
        return c == kByteEndText ? program_.bytemap_range() : program_.bytemap()[static_cast<std::size_t>(c)];
    }

    void add_to_queue(DfaWorkq &queue, std::uint32_t id, std::uint32_t flag) noexcept;
    void state_to_workq(const StateView &state, DfaWorkq &queue) noexcept;
    void run_workq_on_empty_string(DfaWorkq &old_queue, DfaWorkq &new_queue, std::uint32_t flag) noexcept;
    void run_workq_on_byte(DfaWorkq &old_queue, DfaWorkq &new_queue, int c, std::uint32_t flag,
                           bool &is_match) noexcept;
    // Turns a work queue into a state in the spare buffer.
    [[nodiscard]] Outcome workq_to_state(DfaWorkq &queue, DfaWorkq *match_queue, std::uint32_t flag) noexcept;
    // Computes the state after byte c, leaving it in pending_.
    [[nodiscard]] Outcome run_state_on_byte(const StateView &state, int c) noexcept;

    // Slow path of the cached loop: computes s->next[c]. Returns nullptr when the
    // scan has to continue uncached, with pending_ holding the next state.
    [[nodiscard]] DfaState *compute_next(Scan &scan, int c) noexcept;
    void save(const DfaState *state, std::vector<std::uint32_t> &saved, std::uint32_t &flag) noexcept;
    [[nodiscard]] DfaState *restore(const DfaState *original, const std::vector<std::uint32_t> &saved,
                                    std::uint32_t flag) noexcept;

    template<bool kPrefixAccel, bool kEarliest, bool kForward>
    void run_cached(Scan &scan) noexcept;
    template<bool kForward>
    void run_uncached(Scan &scan, bool earliest) noexcept;
    void run(Scan &scan, bool prefix_accel, bool earliest, bool forward) noexcept;

    // One step without the cache. Returns false to stop scanning.
    [[nodiscard]] bool step_uncached(Scan &scan, int c, const std::uint8_t *match_at, bool earliest) noexcept;
    void take_pending() noexcept;
    void note_match(Scan &scan, const StateView &state, const std::uint8_t *match_at) noexcept;
    void finish_text(Scan &scan, int last_byte) noexcept;

    [[nodiscard]] DfaState *start_state(std::size_t index, std::uint32_t flags, bool anchored, StateView &uncached,
                                        bool &use_uncached) noexcept;

    const Program &program_;
    DfaKind kind_;
    DfaWorkspace &ws_;
    WorkBudget &budget_;
    DfaCache &cache_;
    std::uint32_t next_count_;
    DfaWorkq *q0_;
    DfaWorkq *q1_;
    StateView pending_{}; // result of the last computation, in buffers[spare_]
    StateView current_{}; // the current state when running uncached
    std::size_t spare_ = 0; // which buffer the next computation may overwrite
    bool uncached_ = false;
};

// Adds id to the queue, following empty arrows according to flag.
void DfaRun::add_to_queue(DfaWorkq &queue, std::uint32_t id, std::uint32_t flag) noexcept {
    std::uint32_t *stack = ws_.stack.data();
    std::size_t depth = 0;
    stack[depth++] = id;

    while (depth > 0) {
        id = stack[--depth];

        for (;;) {
            if (id == kMark) {
                queue.mark();
                break;
            }
            if (id == 0) {
                break;
            }

            // If already on the queue, there is nothing to do. Not everything added
            // here is kept, but adding it makes this check succeed more often.
            if (queue.contains(id)) {
                break;
            }
            queue.insert_new(id);

            const Inst &ip = program_.inst(id);
            bool advance = false;
            switch (ip.opcode()) {
                case InstOp::ByteRange: // just save these on the queue
                case InstOp::Match:
                    if (!ip.last()) {
                        ++id;
                        advance = true;
                    }
                    break;

                case InstOp::Capture: // the DFA treats captures as no-ops
                case InstOp::Nop:
                    if (!ip.last()) {
                        stack[depth++] = id + 1;
                    }
                    // If this is the [00-FF]* loop at the start of a leftmost-longest
                    // unanchored search, separate with a Mark, so that threads
                    // starting further right are lower priority than current ones.
                    if (ip.opcode() == InstOp::Nop && queue.max_mark() > 0 && id == program_.start_unanchored() &&
                        id != program_.start()) {
                        stack[depth++] = kMark;
                    }
                    id = ip.out();
                    advance = true;
                    break;

                case InstOp::AltMatch:
                    ++id;
                    advance = true;
                    break;

                case InstOp::EmptyWidth:
                    if (!ip.last()) {
                        stack[depth++] = id + 1;
                    }
                    // Continue on if all the required flag bits are there.
                    if ((ip.empty() & ~flag) == 0) {
                        id = ip.out();
                        advance = true;
                    }
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

void DfaRun::state_to_workq(const StateView &state, DfaWorkq &queue) noexcept {
    queue.clear_queue();
    for (std::uint32_t i = 0; i < state.ninst; ++i) {
        const std::uint32_t id = state.inst[i];
        if (id == kMark) {
            queue.mark();
        } else if (id == kMatchSep) {
            break; // nothing after this is an instruction
        } else {
            // Explore from the head of the list.
            add_to_queue(queue, id, state.flag & kFlagEmptyMask);
        }
    }
}

// Runs the queue, processing the empty strings in flag. All the flags have to be
// passed at once: processing ^ and then $ would not match ^$^$^$.
void DfaRun::run_workq_on_empty_string(DfaWorkq &old_queue, DfaWorkq &new_queue, std::uint32_t flag) noexcept {
    new_queue.clear_queue();
    for (std::uint32_t i = 0; i < old_queue.size(); ++i) {
        const std::uint32_t id = old_queue.at(i);
        add_to_queue(new_queue, old_queue.is_mark(id) ? kMark : id, flag);
    }
}

// Runs the queue on byte c followed by any empty strings in flag. Sets is_match if
// the end of the program has been reached.
void DfaRun::run_workq_on_byte(DfaWorkq &old_queue, DfaWorkq &new_queue, int c, std::uint32_t flag,
                               bool &is_match) noexcept {
    new_queue.clear_queue();
    for (std::uint32_t i = 0; i < old_queue.size(); ++i) {
        const std::uint32_t id = old_queue.at(i);
        if (old_queue.is_mark(id)) {
            if (is_match) {
                return;
            }
            new_queue.mark();
            continue;
        }

        const Inst &ip = program_.inst(id);
        switch (ip.opcode()) {
            case InstOp::ByteRange:
                if (!ip.matches(c)) {
                    break;
                }
                add_to_queue(new_queue, ip.out(), flag);
                if (ip.hint() != 0) {
                    // Cancel out the increment at the end of the loop.
                    i += ip.hint() - 1;
                } else {
                    // No hint, so find the end of the current list and skip to it.
                    std::uint32_t last = id;
                    while (!program_.inst(last).last()) {
                        ++last;
                    }
                    i += last - id;
                }
                break;

            case InstOp::Match:
                if (program_.anchor_end() && c != kByteEndText && kind_ != DfaKind::ManyMatch) {
                    break;
                }
                is_match = true;
                if (kind_ == DfaKind::FirstMatch) {
                    return; // a match ends the work on this queue
                }
                break;

            default: // the others were already followed
                break;
        }
    }
}

DfaRun::Outcome DfaRun::workq_to_state(DfaWorkq &queue, DfaWorkq *match_queue, std::uint32_t flag) noexcept {
    std::uint32_t *inst = ws_.buffers[spare_].data();
    std::uint32_t n = 0;
    std::uint32_t need_flags = 0; // flags needed by EmptyWidth instructions
    bool saw_match = false; // the queue contains a guaranteed Match
    bool saw_mark = false;

    for (std::uint32_t i = 0; i < queue.size(); ++i) {
        const std::uint32_t id = queue.at(i);
        if (saw_match && (kind_ == DfaKind::FirstMatch || queue.is_mark(id))) {
            break;
        }
        if (queue.is_mark(id)) {
            if (n > 0 && inst[n - 1] != kMark) {
                saw_mark = true;
                inst[n++] = kMark;
            }
            continue;
        }

        const Inst &ip = program_.inst(id);
        // This state continues to a match whatever the rest of the input is. If it
        // is the highest priority match in play, it is matches all the way from here.
        if (ip.opcode() == InstOp::AltMatch && kind_ != DfaKind::ManyMatch &&
            (kind_ != DfaKind::FirstMatch || (i == 0 && ip.greedy(program_))) &&
            (kind_ != DfaKind::LongestMatch || !saw_mark) && (flag & kFlagMatch) != 0) {
            return Outcome::FullMatch;
        }

        // Record id if it heads its list, which it does if id-1 ends the previous one.
        if (program_.inst(id - 1).last()) {
            inst[n++] = id;
        }
        if (ip.opcode() == InstOp::EmptyWidth) {
            need_flags |= ip.empty();
        }
        if (ip.opcode() == InstOp::Match && !program_.anchor_end()) {
            saw_match = true;
        }
    }
    if (n > 0 && inst[n - 1] == kMark) {
        --n;
    }

    // With no empty-width instructions waiting, the flag bits would never be used.
    // Dropping them reduces the number of distinct states.
    if (need_flags == 0) {
        flag &= kFlagMatch;
    }

    // Nothing left to run makes this a dead state, unless it is a matching one.
    if (n == 0 && flag == 0) {
        return Outcome::Dead;
    }

    // In longest match mode a state is a sequence of unordered sets separated by
    // Marks. Sort each to reduce the number of distinct states stored.
    if (kind_ == DfaKind::LongestMatch) {
        std::uint32_t *ip = inst;
        std::uint32_t *ep = inst + n;
        while (ip < ep) {
            std::uint32_t *mark = std::find(ip, ep, kMark);
            std::sort(ip, mark);
            ip = mark < ep ? mark + 1 : mark;
        }
    }
    // In many match mode the state is one unordered set, for the same reason.
    if (kind_ == DfaKind::ManyMatch) {
        std::sort(inst, inst + n);
    }

    // Append the match ids if necessary.
    if (match_queue != nullptr) {
        inst[n++] = kMatchSep;
        for (std::uint32_t i = 0; i < match_queue->size(); ++i) {
            const std::uint32_t id = match_queue->at(i);
            if (!match_queue->is_mark(id) && program_.inst(id).opcode() == InstOp::Match) {
                inst[n++] = program_.inst(id).match_id();
            }
        }
    }

    // Save the needed empty-width flags in the top bits for use later.
    pending_ = StateView{.inst = inst, .ninst = n, .flag = flag | (need_flags << kFlagNeedShift)};
    return Outcome::State;
}

DfaRun::Outcome DfaRun::run_state_on_byte(const StateView &state, int c) noexcept {
    state_to_workq(state, *q0_);

    // The empty-width things around this byte. Before it there are the flags in the
    // state; after it there is nothing yet.
    const std::uint32_t need_flag = state.flag >> kFlagNeedShift;
    std::uint32_t before_flag = state.flag & kFlagEmptyMask;
    const std::uint32_t old_before_flag = before_flag;
    std::uint32_t after_flag = 0;

    if (c == '\n') {
        // Insert the implicit $ and ^ around \n.
        before_flag |= kEmptyEndLine;
        after_flag |= kEmptyBeginLine;
    }
    if (c == kByteEndText) {
        // Insert the implicit $ and \z before the fake end of text byte.
        before_flag |= kEmptyEndLine | kEmptyEndText;
    }

    // kFlagLastWord says whether the previous byte was a word character.
    const bool is_last_word = (state.flag & kFlagLastWord) != 0;
    const bool is_word = c != kByteEndText && is_word_char(static_cast<std::uint8_t>(c));
    before_flag |= is_word == is_last_word ? kEmptyNonWordBoundary : kEmptyWordBoundary;

    // Only worth rerunning on the empty string if there are new, useful flags.
    if ((before_flag & ~old_before_flag & need_flag) != 0) {
        run_workq_on_empty_string(*q0_, *q1_, before_flag);
        std::swap(q0_, q1_);
    }
    bool is_match = false;
    run_workq_on_byte(*q0_, *q1_, c, after_flag, is_match);
    std::swap(q0_, q1_);

    std::uint32_t flag = after_flag;
    if (is_match) {
        flag |= kFlagMatch;
    }
    if (is_word) {
        flag |= kFlagLastWord;
    }
    return workq_to_state(*q0_, is_match && kind_ == DfaKind::ManyMatch ? q1_ : nullptr, flag);
}

void DfaRun::save(const DfaState *state, std::vector<std::uint32_t> &saved, std::uint32_t &flag) noexcept {
    if (is_special(state)) {
        return;
    }
    saved.assign(state->inst, state->inst + state->ninst);
    flag = state->flag;
}

DfaState *DfaRun::restore(const DfaState *original, const std::vector<std::uint32_t> &saved,
                          std::uint32_t flag) noexcept {
    if (is_special(original)) {
        return const_cast<DfaState *>(original); // NOLINT(cppcoreguidelines-pro-type-const-cast) a tag, not an object
    }
    return cache_.intern(ws_, saved.data(), static_cast<std::uint32_t>(saved.size()), flag, next_count_);
}

DfaState *DfaRun::compute_next(Scan &scan, int c) noexcept {
    DfaState *state = scan.state;
    const StateView view = view_of(state);
    if (!charge(std::uint64_t{view.ninst} + 1)) {
        scan.exhausted = true;
        scan.stopped = true;
        return dead_state();
    }

    DfaState *next = nullptr;
    switch (run_state_on_byte(view, c)) {
        case Outcome::Dead:
            next = dead_state();
            break;
        case Outcome::FullMatch:
            next = full_match_state();
            break;
        case Outcome::State:
            next = cache_.intern(ws_, pending_.inst, pending_.ninst, pending_.flag, next_count_);
            break;
    }
    if (next != nullptr) {
        state->next()[byte_class(c)] = next;
        return next;
    }

    // Out of memory. If the cache refilled this fast, stop using it.
    const std::uint8_t *p = scan.p;
    if (scan.reset_p != nullptr) {
        const auto distance = static_cast<std::size_t>(p > scan.reset_p ? p - scan.reset_p : scan.reset_p - p);
        if (distance < kMinBytesPerState * cache_.state_count()) {
            return nullptr;
        }
    }
    scan.reset_p = p;

    // Keep the states the scan is using across the reset. pending_ is not in the
    // cache, so it survives on its own.
    std::uint32_t start_flag = 0;
    std::uint32_t state_flag = 0;
    const DfaState *old_start = scan.start;
    save(old_start, ws_.saved_start, start_flag);
    save(state, ws_.saved_state, state_flag);
    cache_.reset(ws_);

    // A resumed stream has no start state to keep.
    scan.start = restore(old_start, ws_.saved_start, start_flag);
    scan.state = restore(state, ws_.saved_state, state_flag);
    if ((old_start != nullptr && scan.start == nullptr) || scan.state == nullptr) {
        return nullptr;
    }

    next = cache_.intern(ws_, pending_.inst, pending_.ninst, pending_.flag, next_count_);
    if (next != nullptr) {
        scan.state->next()[byte_class(c)] = next;
    }
    return next;
}

// Adopts pending_ as the current uncached state.
void DfaRun::take_pending() noexcept {
    current_ = pending_;
    spare_ = 1 - spare_;
}

void DfaRun::note_match(Scan &scan, const StateView &state, const std::uint8_t *match_at) noexcept {
    scan.matched = true;
    scan.last_match = match_at;
    collect_matches(state, scan.matches);
}

bool DfaRun::step_uncached(Scan &scan, int c, const std::uint8_t *match_at, bool earliest) noexcept {
    if (!charge(std::uint64_t{current_.ninst} + 1)) {
        scan.exhausted = true;
        scan.stopped = true;
        return false;
    }
    switch (run_state_on_byte(current_, c)) {
        case Outcome::Dead:
            scan.stopped = true;
            return false;
        case Outcome::FullMatch:
            scan.matched = true;
            scan.last_match = scan.ep;
            scan.stopped = true;
            return false;
        case Outcome::State:
            break;
    }
    take_pending();
    if (current_.is_match()) {
        note_match(scan, current_, match_at);
        if (earliest) {
            scan.stopped = true;
            return false;
        }
    }
    return true;
}

template<bool kForward>
void DfaRun::run_uncached(Scan &scan, bool earliest) noexcept {
    while (scan.p != scan.ep) {
        const int c = kForward ? *scan.p++ : *--scan.p;
        // Matches are noticed one byte late.
        if (!step_uncached(scan, c, kForward ? scan.p - 1 : scan.p + 1, earliest)) {
            return;
        }
    }
}

// The search loop, specialized because checking these once per byte would slow it
// down: with one set of flags it is just s = s->next[bytemap[c]].
template<bool kPrefixAccel, bool kEarliest, bool kForward>
void DfaRun::run_cached(Scan &scan) noexcept {
    const std::uint8_t *bytemap = program_.bytemap().data();
    const int prefix_byte = program_.prefix_byte();
    const std::uint8_t *p = scan.p;
    const std::uint8_t *const ep = scan.ep;
    DfaState *s = scan.state;

    while (p != ep) {
        if (kPrefixAccel && s == scan.start) {
            // In the start state, the only way out is the prefix.
            const void *found = std::memchr(p, prefix_byte, static_cast<std::size_t>(ep - p));
            if (found == nullptr) {
                p = ep;
                break;
            }
            p = static_cast<const std::uint8_t *>(found);
        }

        const int c = kForward ? *p++ : *--p;
        DfaState *next = s->next()[bytemap[c]];
        if (next == nullptr) {
            scan.p = p;
            scan.state = s;
            next = compute_next(scan, c);
            if (scan.exhausted) {
                return;
            }
            if (next == nullptr) {
                // Carry on without the cache, from the state just computed.
                uncached_ = true;
                take_pending();
                if (current_.is_match()) {
                    note_match(scan, current_, kForward ? p - 1 : p + 1);
                    if (kEarliest) {
                        scan.stopped = true;
                        return;
                    }
                }
                run_uncached<kForward>(scan, kEarliest);
                return;
            }
            // A reset moves the states.
            s = scan.state;
        }

        if (is_special(next)) {
            if (next == full_match_state()) {
                scan.matched = true;
                scan.last_match = ep;
            }
            scan.p = p;
            scan.state = s;
            scan.stopped = true;
            return;
        }

        s = next;
        if ((s->flag & kFlagMatch) != 0) {
            // The DFA notices a match one byte late.
            note_match(scan, view_of(s), kForward ? p - 1 : p + 1);
            if (kEarliest) {
                scan.p = p;
                scan.state = s;
                scan.stopped = true;
                return;
            }
        }
    }

    scan.p = p;
    scan.state = s;
}

void DfaRun::run(Scan &scan, bool prefix_accel, bool earliest, bool forward) noexcept {
    if (uncached_) {
        if (forward) {
            run_uncached<true>(scan, earliest);
        } else {
            run_uncached<false>(scan, earliest);
        }
        return;
    }
    // A reversed program has no prefix, so prefix_accel implies forward.
    if (prefix_accel) {
        earliest ? run_cached<true, true, true>(scan) : run_cached<true, false, true>(scan);
    } else if (forward) {
        earliest ? run_cached<false, true, true>(scan) : run_cached<false, false, true>(scan);
    } else {
        earliest ? run_cached<false, true, false>(scan) : run_cached<false, false, false>(scan);
    }
}

// Processes one more byte to see if it triggers a match: matches are delayed by one
// byte, to accommodate conditions that depend on the next byte, like $ and \b.
void DfaRun::finish_text(Scan &scan, int last_byte) noexcept {
    const std::uint8_t *const match_at = scan.p;
    if (uncached_) {
        (void) step_uncached(scan, last_byte, match_at, false);
        return;
    }

    DfaState *next = scan.state->next()[byte_class(last_byte)];
    if (next == nullptr) {
        next = compute_next(scan, last_byte);
        if (scan.exhausted) {
            return;
        }
        if (next == nullptr) {
            uncached_ = true;
            take_pending();
            if (current_.is_match()) {
                note_match(scan, current_, match_at);
            }
            return;
        }
    }
    if (is_special(next)) {
        if (next == full_match_state()) {
            scan.matched = true;
            scan.last_match = scan.ep;
        }
        return;
    }
    scan.state = next;
    if ((next->flag & kFlagMatch) != 0) {
        note_match(scan, view_of(next), match_at);
    }
}

DfaState *DfaRun::start_state(std::size_t index, std::uint32_t flags, bool anchored, StateView &uncached,
                              bool &use_uncached) noexcept {
    DfaState *&slot = cache_.start(index);
    if (slot != nullptr) {
        return slot;
    }

    q0_->clear_queue();
    add_to_queue(*q0_, anchored ? program_.start() : program_.start_unanchored(), flags);
    switch (workq_to_state(*q0_, nullptr, flags)) {
        case Outcome::Dead:
            slot = dead_state();
            return slot;
        case Outcome::FullMatch:
            slot = full_match_state();
            return slot;
        case Outcome::State:
            break;
    }

    DfaState *state = cache_.intern(ws_, pending_.inst, pending_.ninst, pending_.flag, next_count_);
    if (state == nullptr) {
        cache_.reset(ws_);
        state = cache_.intern(ws_, pending_.inst, pending_.ninst, pending_.flag, next_count_);
    }
    if (state == nullptr) {
        // Not even room for this one state.
        take_pending();
        uncached = current_;
        use_uncached = true;
        return nullptr;
    }
    cache_.start(index) = state;
    return state;
}

DfaResult DfaRun::search(const DfaRequest &request, bool anchored, bool earliest, bool forward) noexcept {
    const std::string_view context = request.context;
    const std::size_t begin = request.begin;
    const std::size_t end = request.end;
    const auto *base = reinterpret_cast<const std::uint8_t *>(context.data());
    const auto offset_of = [base](const std::uint8_t *at) { return static_cast<std::size_t>(at - base); };

    // Determine the empty-width flags in effect where the search starts.
    std::size_t index = 0;
    std::uint32_t flags = 0;
    const bool at_text_edge = forward ? begin == 0 : end == context.size();
    if (at_text_edge) {
        index = kStartBeginText;
        flags = kEmptyBeginText | kEmptyBeginLine;
    } else {
        const char neighbour = forward ? context[begin - 1] : context[end];
        if (neighbour == '\n') {
            index = kStartBeginLine;
            flags = kEmptyBeginLine;
        } else if (is_word_char(static_cast<std::uint8_t>(neighbour))) {
            index = kStartAfterWordChar;
            flags = kFlagLastWord;
        } else {
            index = kStartAfterNonWordChar;
        }
    }
    if (anchored) {
        index += kStartAnchored;
    }

    Scan scan;
    scan.p = forward ? base + begin : base + end;
    scan.ep = forward ? base + end : base + begin;
    scan.matches = request.matches;

    // Bytes are charged up front, which costs the inner loop nothing.
    if (!charge(end - begin)) {
        return {.status = MatchStatus::BudgetExhausted, .position = 0};
    }

    StateView uncached;
    DfaState *start = start_state(index, flags, anchored, uncached, uncached_);
    if (start == dead_state()) {
        return {.status = MatchStatus::NoMatch, .position = 0};
    }
    if (start == full_match_state()) {
        return {.status = MatchStatus::Match, .position = offset_of(forward == earliest ? base + begin : base + end)};
    }

    scan.start = start;
    scan.state = start;
    const StateView first = uncached_ ? uncached : view_of(start);
    if (first.is_match()) {
        note_match(scan, first, scan.p);
        if (earliest) {
            return {.status = MatchStatus::Match, .position = offset_of(scan.last_match)};
        }
    }

    // A prefix only helps if leaving the start state takes that one byte, so not when
    // anchored and not when flags are involved.
    const bool prefix_accel =
            !uncached_ && forward && program_.prefix_byte() >= 0 && !anchored && (start->flag >> kFlagNeedShift) == 0;
    run(scan, prefix_accel, earliest, forward);

    if (!scan.stopped) {
        // What follows the text decides what the next byte is, if not its end.
        const bool at_end = forward ? end == context.size() : begin == 0;
        int last_byte = kByteEndText;
        if (!at_end) {
            last_byte = static_cast<unsigned char>(forward ? context[end] : context[begin - 1]);
        }
        finish_text(scan, last_byte);
    }

    if (scan.exhausted) {
        return {.status = MatchStatus::BudgetExhausted, .position = 0};
    }
    if (!scan.matched) {
        return {.status = MatchStatus::NoMatch, .position = 0};
    }
    return {.status = MatchStatus::Match, .position = offset_of(scan.last_match)};
}

MatchStatus DfaRun::feed(DfaCursor &cursor, std::string_view chunk, MatchSet &matches) noexcept {
    if (cursor.dead) {
        return MatchStatus::NoMatch;
    }
    if (!charge(chunk.size())) {
        return MatchStatus::BudgetExhausted;
    }

    Scan scan;
    scan.matches = &matches;
    const auto *data = reinterpret_cast<const std::uint8_t *>(chunk.data());
    scan.p = data;
    scan.ep = data + chunk.size();

    if (!cursor.started) {
        StateView uncached;
        DfaState *start = start_state(kStartBeginText + kStartAnchored, kEmptyBeginText | kEmptyBeginLine, true,
                                      uncached, uncached_);
        cursor.started = true;
        // ManyMatch never produces a full match state, which needs a single winner.
        FIBER_ASSERT(start != full_match_state());
        if (start == dead_state()) {
            cursor.dead = true;
            return MatchStatus::NoMatch;
        }
        scan.start = start;
        scan.state = start;
        const StateView first = uncached_ ? uncached : view_of(start);
        if (first.is_match()) {
            collect_matches(first, &matches);
        }
    } else if (cursor.epoch == cache_.epoch() && cursor.state != nullptr) {
        scan.state = cursor.state;
    } else {
        // The cache moved on since the last chunk, so rebuild the state in it.
        scan.state = cache_.intern(ws_, cursor.inst.data(), static_cast<std::uint32_t>(cursor.inst.size()), cursor.flag,
                                   next_count_);
        if (scan.state == nullptr) {
            uncached_ = true;
            current_ = StateView{.inst = cursor.inst.data(),
                                 .ninst = static_cast<std::uint32_t>(cursor.inst.size()),
                                 .flag = cursor.flag};
        }
    }

    run(scan, false, false, true);
    if (scan.exhausted) {
        return MatchStatus::BudgetExhausted;
    }
    if (scan.stopped) {
        cursor.dead = true;
        return MatchStatus::NoMatch;
    }

    // Without any bytes to run, current_ is still the cursor's own storage.
    const StateView state = uncached_ ? current_ : view_of(scan.state);
    cursor.flag = state.flag;
    if (state.inst != cursor.inst.data()) {
        cursor.inst.assign(state.inst, state.inst + state.ninst);
    }
    cursor.state = uncached_ ? nullptr : scan.state;
    cursor.epoch = cache_.epoch();
    return MatchStatus::NoMatch;
}

MatchStatus DfaRun::finish(DfaCursor &cursor, MatchSet &matches) noexcept {
    if (cursor.dead) {
        return MatchStatus::NoMatch;
    }

    // An empty stream has not started yet, so feed it nothing first.
    if (!cursor.started) {
        const MatchStatus status = feed(cursor, {}, matches);
        if (status != MatchStatus::NoMatch || cursor.dead) {
            return status;
        }
    }

    Scan scan;
    scan.matches = &matches;
    if (cursor.epoch == cache_.epoch() && cursor.state != nullptr) {
        scan.state = cursor.state;
    } else {
        scan.state = cache_.intern(ws_, cursor.inst.data(), static_cast<std::uint32_t>(cursor.inst.size()), cursor.flag,
                                   next_count_);
        if (scan.state == nullptr) {
            uncached_ = true;
            current_ = StateView{.inst = cursor.inst.data(),
                                 .ninst = static_cast<std::uint32_t>(cursor.inst.size()),
                                 .flag = cursor.flag};
        }
    }
    finish_text(scan, kByteEndText);
    cursor.dead = true;
    return scan.exhausted ? MatchStatus::BudgetExhausted : MatchStatus::NoMatch;
}

} // namespace

DfaState *DfaCache::intern(DfaWorkspace &workspace, const std::uint32_t *inst, std::uint32_t ninst, std::uint32_t flag,
                           std::uint32_t next_count) noexcept {
    const std::uint32_t hash = hash_state(inst, ninst, flag);

    if (!table_.empty()) {
        const std::size_t mask = table_.size() - 1;
        for (std::size_t slot = hash & mask;; slot = (slot + 1) & mask) {
            DfaState *state = table_[slot];
            if (state == nullptr) {
                break;
            }
            if (state->hash == hash && state->flag == flag && state->ninst == ninst &&
                std::equal(inst, inst + ninst, state->inst)) {
                return state;
            }
        }
    }

    // Keep the table at most half full.
    if (2 * (count_ + 1) > table_.size() && !grow_table(workspace)) {
        return nullptr;
    }

    const std::size_t next_bytes = std::size_t{next_count} * sizeof(DfaState *);
    const std::size_t inst_bytes = std::size_t{ninst} * sizeof(std::uint32_t);
    const std::size_t size = sizeof(DfaState) + next_bytes + inst_bytes;
    if (!workspace.reserve(this, size)) {
        return nullptr;
    }
    void *memory = pool_.alloc(size, alignof(DfaState));
    if (memory == nullptr) {
        workspace.release(size);
        return nullptr;
    }
    bytes_ += size;

    auto *state = new (memory) DfaState;
    std::fill_n(state->next(), next_count, nullptr);
    auto *copy = reinterpret_cast<std::uint32_t *>(state->next() + next_count);
    if (ninst != 0) {
        std::memcpy(copy, inst, inst_bytes);
    }
    state->inst = copy;
    state->ninst = ninst;
    state->flag = flag;
    state->hash = hash;

    const std::size_t mask = table_.size() - 1;
    std::size_t slot = hash & mask;
    while (table_[slot] != nullptr) {
        slot = (slot + 1) & mask;
    }
    table_[slot] = state;
    ++count_;
    return state;
}

bool DfaCache::grow_table(DfaWorkspace &workspace) noexcept {
    const std::size_t new_size = table_.empty() ? 64 : table_.size() * 2;
    const std::size_t extra = (new_size - table_.size()) * sizeof(DfaState *);
    if (!workspace.reserve(this, extra)) {
        return false;
    }
    bytes_ += extra;

    std::vector<DfaState *> table(new_size, nullptr);
    const std::size_t mask = new_size - 1;
    for (DfaState *state: table_) {
        if (state == nullptr) {
            continue;
        }
        std::size_t slot = state->hash & mask;
        while (table[slot] != nullptr) {
            slot = (slot + 1) & mask;
        }
        table[slot] = state;
    }
    table_ = std::move(table);
    return true;
}

void DfaCache::reset(DfaWorkspace &workspace) noexcept {
    workspace.release(bytes_);
    bytes_ = 0;
    count_ = 0;
    pool_.reset();
    table_.clear();
    start_.fill(nullptr);
    epoch_ = next_epoch();
}

DfaCache &DfaWorkspace::cache_for(const Program &program, DfaKind kind) {
    const std::uint64_t key = (program.id() * 4U) + static_cast<std::uint64_t>(kind);
    const auto it = caches_.find(key);
    if (it != caches_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second);
        return lru_.front().cache;
    }
    lru_.emplace_front(key, next_epoch());
    caches_.emplace(key, lru_.begin());
    return lru_.front().cache;
}

bool DfaWorkspace::reserve(const DfaCache *self, std::size_t bytes) noexcept {
    // The cache at the back may belong to a Regex that is long gone: nothing else
    // ever drops those.
    while (used_ + bytes > budget_) {
        if (lru_.empty() || &lru_.back().cache == self) {
            return false;
        }
        used_ -= lru_.back().cache.bytes();
        caches_.erase(lru_.back().key);
        lru_.pop_back();
    }
    used_ += bytes;
    return true;
}

DfaResult dfa_search(const Program &program, DfaWorkspace &workspace, const DfaRequest &request,
                     WorkBudget &budget) noexcept {
    if (program.start() == 0) {
        return {};
    }

    // The anchors of a reversed program are the other way around.
    const bool caret = program.reversed() ? program.anchor_end() : program.anchor_start();
    const bool dollar = program.reversed() ? program.anchor_start() : program.anchor_end();
    if (caret && request.begin != 0) {
        return {};
    }
    if (dollar && request.end != request.context.size()) {
        return {};
    }

    // A full match is an anchored longest match that covers all of the text.
    DfaSearchKind kind = request.kind;
    const bool anchored = request.anchored || program.anchor_start() || kind == DfaSearchKind::FullMatch;
    bool end_match = false;
    if (kind != DfaSearchKind::ManyMatch && (kind == DfaSearchKind::FullMatch || program.anchor_end())) {
        end_match = true;
        kind = DfaSearchKind::LongestMatch;
    }

    // If the caller doesn't care where the match is, stop at the first one found.
    bool earliest = false;
    if (kind == DfaSearchKind::ManyMatch) {
        earliest = request.matches == nullptr;
    } else if (!request.want_position && !end_match) {
        earliest = true;
        kind = DfaSearchKind::LongestMatch;
    }

    DfaKind dfa_kind = DfaKind::LongestMatch;
    if (kind == DfaSearchKind::ManyMatch) {
        dfa_kind = DfaKind::ManyMatch;
    } else if (kind == DfaSearchKind::FirstMatch) {
        dfa_kind = DfaKind::FirstMatch;
    }
    DfaRun run(program, dfa_kind, workspace, budget);
    const DfaResult result = run.search(request, anchored, earliest, !program.reversed());
    if (result.status != MatchStatus::Match) {
        return result;
    }
    if (end_match && result.position != (program.reversed() ? request.begin : request.end)) {
        return {};
    }
    return result;
}

MatchStatus dfa_feed(const Program &program, DfaWorkspace &workspace, DfaCursor &cursor, std::string_view chunk,
                     MatchSet &matches, WorkBudget &budget) noexcept {
    if (program.start() == 0) {
        return MatchStatus::NoMatch;
    }
    DfaRun run(program, DfaKind::ManyMatch, workspace, budget);
    return run.feed(cursor, chunk, matches);
}

MatchStatus dfa_finish(const Program &program, DfaWorkspace &workspace, DfaCursor &cursor, MatchSet &matches,
                       WorkBudget &budget) noexcept {
    if (program.start() == 0) {
        return MatchStatus::NoMatch;
    }
    DfaRun run(program, DfaKind::ManyMatch, workspace, budget);
    return run.finish(cursor, matches);
}

} // namespace fiber::regex::detail
