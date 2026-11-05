// Ported from RE2's bitstate.cc (see UPSTREAM.md).
#include "regex/BitState.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include <fiber/common/Assert.h>

namespace fiber::regex::detail {

namespace {

constexpr std::size_t kVisitedBits = 64;
constexpr std::size_t kUnset = Span::npos;

class BitState {
public:
    BitState(const Program &program, BitStateWorkspace &workspace, const SearchRequest &request,
             WorkBudget &budget) noexcept :
        program_(program), ws_(workspace), request_(request), budget_(budget), context_(request.context),
        begin_(request.begin), end_(request.end), list_heads_(program.list_heads().data()) {}

    [[nodiscard]] MatchStatus search() noexcept;

private:
    enum class Flow : std::uint8_t { Loop, CheckAndLoop, Next, Stop };

    // Whether (list, pos) still needs a visit. Remembers that it got one.
    [[nodiscard]] bool should_visit(std::uint32_t list, std::size_t pos) noexcept {
        const std::size_t n = (list * (end_ - begin_ + 1)) + (pos - begin_);
        std::uint64_t &word = ws_.visited[n / kVisitedBits];
        const std::uint64_t bit = std::uint64_t{1} << (n % kVisitedBits);
        if ((word & bit) != 0) {
            return false;
        }
        word |= bit;
        return true;
    }

    void push(std::int32_t id, std::size_t pos) noexcept;
    [[nodiscard]] bool try_search(std::uint32_t id0, std::size_t pos0) noexcept;
    void record_match() noexcept;

    const Program &program_;
    BitStateWorkspace &ws_;
    const SearchRequest &request_;
    WorkBudget &budget_;
    std::string_view context_;
    std::size_t begin_;
    std::size_t end_;
    const std::uint16_t *list_heads_;
    std::size_t job_count_ = 0;
    bool longest_ = false;
    bool end_match_ = false;
    bool exhausted_ = false;
    bool have_match_ = false;
    std::size_t best_end_ = 0;
};

// Pushes (id, pos). A run of the same id at consecutive positions is stored as
// one job: that is what x* over a long text produces.
void BitState::push(std::int32_t id, std::size_t pos) noexcept {
    // A negative id is undoing a capture, which must stay a job of its own.
    if (id >= 0 && job_count_ > 0) {
        BitStateWorkspace::Job &top = ws_.jobs[job_count_ - 1];
        if (id == top.id && pos == top.pos + static_cast<std::size_t>(top.rle) + 1 &&
            top.rle < std::numeric_limits<std::int32_t>::max()) {
            ++top.rle;
            return;
        }
    }
    if (job_count_ == ws_.jobs.size()) {
        ws_.jobs.resize(ws_.jobs.size() * 2);
    }
    ws_.jobs[job_count_++] = BitStateWorkspace::Job{.id = id, .rle = 0, .pos = pos};
}

void BitState::record_match() noexcept {
    for (std::size_t i = 0; i < request_.captures.size(); ++i) {
        Span &span = request_.captures[i];
        span.begin = ws_.capture[2 * i];
        span.end = ws_.capture[(2 * i) + 1];
        if (span.end == kUnset) {
            span.begin = kUnset;
        }
    }
}

// Tries a search from instruction id0 at pos0. Returns whether to stop looking.
bool BitState::try_search(std::uint32_t id0, std::size_t pos0) noexcept {
    bool matched = false;
    job_count_ = 0;
    // push() does not check should_visit(), so check here.
    if (should_visit(list_heads_[id0], pos0)) {
        push(static_cast<std::int32_t>(id0), pos0);
    }

    while (job_count_ > 0) {
        --job_count_;
        BitStateWorkspace::Job &job = ws_.jobs[job_count_];
        const std::int32_t raw_id = job.id;
        std::size_t pos = job.pos;

        if (raw_id < 0) {
            // Undo the capture.
            ws_.capture[program_.inst(static_cast<std::uint32_t>(-raw_id)).cap()] = pos;
            continue;
        }

        if (job.rle > 0) {
            pos += static_cast<std::size_t>(job.rle);
            // Revive the job on the stack, one position shorter.
            --job.rle;
            ++job_count_;
        }

        auto id = static_cast<std::uint32_t>(raw_id);
        for (;;) {
            if (budget_.remaining == 0) {
                exhausted_ = true;
                return true;
            }
            --budget_.remaining;

            const Inst &ip = program_.inst(id);
            Flow flow = Flow::Stop;

            switch (ip.opcode()) {
                case InstOp::Fail:
                    break;

                case InstOp::AltMatch:
                    if (ip.greedy(program_)) {
                        // out1 is the Match instruction.
                        id = ip.out1();
                        pos = end_;
                        flow = Flow::Loop;
                    } else if (longest_) {
                        // Non-greedy, so out is the Match instruction.
                        id = ip.out();
                        pos = end_;
                        flow = Flow::Loop;
                    } else {
                        flow = Flow::Next;
                    }
                    break;

                case InstOp::ByteRange: {
                    const int c = pos < end_ ? static_cast<unsigned char>(context_[pos]) : -1;
                    if (!ip.matches(c)) {
                        flow = Flow::Next;
                        break;
                    }
                    if (ip.hint() != 0) {
                        push(static_cast<std::int32_t>(id + ip.hint()), pos); // try the next when done
                    }
                    id = ip.out();
                    ++pos;
                    flow = Flow::CheckAndLoop;
                    break;
                }

                case InstOp::Capture:
                    if (!ip.last()) {
                        push(static_cast<std::int32_t>(id + 1), pos);
                    }
                    if (ip.cap() < ws_.capture.size()) {
                        // Capture pos, saving the old value to undo when done.
                        push(-static_cast<std::int32_t>(id), ws_.capture[ip.cap()]);
                        ws_.capture[ip.cap()] = pos;
                    }
                    id = ip.out();
                    flow = Flow::CheckAndLoop;
                    break;

                case InstOp::EmptyWidth:
                    if ((ip.empty() & ~Program::empty_flags(context_, pos)) != 0) {
                        flow = Flow::Next;
                        break;
                    }
                    if (!ip.last()) {
                        push(static_cast<std::int32_t>(id + 1), pos);
                    }
                    id = ip.out();
                    flow = Flow::CheckAndLoop;
                    break;

                case InstOp::Nop:
                    if (!ip.last()) {
                        push(static_cast<std::int32_t>(id + 1), pos);
                    }
                    id = ip.out();
                    flow = Flow::CheckAndLoop;
                    break;

                case InstOp::Match:
                    if (end_match_ && pos != end_) {
                        flow = Flow::Next;
                        break;
                    }
                    // If the caller doesn't care where the match is, stop here.
                    if (request_.captures.empty()) {
                        have_match_ = true;
                        return true;
                    }

                    // Only the end point needs comparing: this entire call is
                    // considering one start position.
                    matched = true;
                    ws_.capture[1] = pos;
                    if (!have_match_ || (longest_ && pos > best_end_)) {
                        have_match_ = true;
                        best_end_ = pos;
                        record_match();
                    }
                    // A first match is final, and so is one that used all the text.
                    if (!longest_ || pos == end_) {
                        return true;
                    }
                    // Otherwise keep going in the hope of a longer match. There is
                    // no should_visit() check, as execution stays in the same list.
                    flow = Flow::Next;
                    break;

                case InstOp::Alt:
                    FIBER_PANIC("regex: Alt survived flattening");
            }

            if (flow == Flow::Loop) {
                continue;
            }
            if (flow == Flow::CheckAndLoop) {
                if (should_visit(list_heads_[id], pos)) {
                    continue;
                }
                break;
            }
            if (flow == Flow::Next && !ip.last()) {
                ++id;
                continue;
            }
            break;
        }
    }
    return matched;
}

MatchStatus BitState::search() noexcept {
    if (program_.anchor_start() && begin_ != 0) {
        return MatchStatus::NoMatch;
    }
    if (program_.anchor_end() && end_ != context_.size()) {
        return MatchStatus::NoMatch;
    }
    const bool anchored = request_.anchored || request_.full_match || program_.anchor_start();
    end_match_ = request_.full_match || program_.anchor_end();
    longest_ = request_.longest || end_match_;

    // Size the workspace. All of this is a no-op on a warm one, except for
    // clearing the visited bits, which has to scale with the text anyway.
    const std::size_t bits = std::size_t{program_.list_count()} * (end_ - begin_ + 1);
    ws_.visited.assign((bits + kVisitedBits - 1) / kVisitedBits, 0);
    ws_.capture.assign(2 * std::max<std::size_t>(request_.captures.size(), 1), kUnset);
    if (ws_.jobs.empty()) {
        ws_.jobs.resize(64);
    }

    const std::uint32_t start = program_.start();
    if (anchored) {
        ws_.capture[0] = begin_;
        (void) try_search(start, begin_);
    } else {
        // Unanchored: try each position, including the empty string at the end.
        // That looks quadratic, but visited is not cleared between calls, so no
        // work is duplicated.
        const int prefix_byte = program_.prefix_byte();
        bool stop = false;
        for (std::size_t pos = begin_; pos <= end_ && !stop; ++pos) {
            // With a required prefix, skip ahead to where a match could start.
            if (pos < end_ && prefix_byte >= 0) {
                const void *found = std::memchr(context_.data() + pos, prefix_byte, end_ - pos);
                pos = found == nullptr ? end_
                                       : static_cast<std::size_t>(static_cast<const char *>(found) - context_.data());
            }
            ws_.capture[0] = pos;
            stop = try_search(start, pos);
        }
    }

    if (exhausted_) {
        return MatchStatus::BudgetExhausted;
    }
    return have_match_ ? MatchStatus::Match : MatchStatus::NoMatch;
}

} // namespace

MatchStatus bit_state_search(const Program &program, BitStateWorkspace &workspace, const SearchRequest &request,
                             WorkBudget &budget) noexcept {
    if (program.start() == 0) {
        return MatchStatus::NoMatch;
    }
    BitState search(program, workspace, request, budget);
    return search.search();
}

} // namespace fiber::regex::detail
