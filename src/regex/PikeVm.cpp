// Ported from RE2's nfa.cc (see UPSTREAM.md). Positions are offsets rather than
// pointers, and all memory comes from the caller's workspace.
#include "regex/PikeVm.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::regex::detail {

namespace {

constexpr std::size_t kUnset = Span::npos;

constexpr PikeWorkspace::AddState visit(std::uint32_t id) noexcept { return PikeWorkspace::AddState{.id = id}; }

class PikeVm {
public:
    PikeVm(const Program &program, PikeWorkspace &workspace, const SearchRequest &request, WorkBudget &budget) noexcept
        :
        program_(program), ws_(workspace), request_(request), budget_(budget), context_(request.context),
        end_(request.end) {}

    [[nodiscard]] MatchStatus search() noexcept;

private:
    using Queue = SparseArray<ThreadId>;

    [[nodiscard]] std::size_t *capture_of(ThreadId thread) noexcept {
        return ws_.captures.data() + (static_cast<std::size_t>(thread) * ncapture_);
    }

    void copy_capture(std::size_t *dst, const std::size_t *src) const noexcept {
        std::memcpy(dst, src, ncapture_ * sizeof(std::size_t));
    }

    [[nodiscard]] ThreadId alloc_thread() noexcept {
        if (free_list_ != kNoThread) {
            const ThreadId thread = free_list_;
            free_list_ = ws_.threads[static_cast<std::size_t>(thread)].next;
            ws_.threads[static_cast<std::size_t>(thread)].ref = 1;
            return thread;
        }
        const auto thread = static_cast<ThreadId>(ws_.threads.size());
        ws_.threads.push_back(PikeWorkspace::Thread{.ref = 1, .next = kNoThread});
        ws_.captures.resize(ws_.captures.size() + ncapture_);
        return thread;
    }

    ThreadId incref(ThreadId thread) noexcept {
        ++ws_.threads[static_cast<std::size_t>(thread)].ref;
        return thread;
    }

    void decref(ThreadId thread) noexcept {
        PikeWorkspace::Thread &t = ws_.threads[static_cast<std::size_t>(thread)];
        if (--t.ref == 0) {
            t.next = free_list_;
            free_list_ = thread;
        }
    }

    void add_to_queue(Queue &queue, std::uint32_t id0, int c, std::size_t pos, ThreadId t0) noexcept;
    [[nodiscard]] std::uint32_t step(Queue &runq, Queue &nextq, int c, std::size_t pos) noexcept;
    void drain_from(Queue &runq, std::uint32_t position) noexcept;
    void finish_shortcut(std::uint32_t id) noexcept;

    [[nodiscard]] int byte_at(std::size_t pos) const noexcept {
        return pos < end_ ? static_cast<unsigned char>(context_[pos]) : -1;
    }

    const Program &program_;
    PikeWorkspace &ws_;
    const SearchRequest &request_;
    WorkBudget &budget_;
    std::string_view context_;
    std::size_t end_;
    std::size_t ncapture_ = 2;
    ThreadId free_list_ = kNoThread;
    bool longest_ = false;
    bool end_match_ = false; // the match must end at end_
    bool matched_ = false;
};

// Follows all empty arrows from id0 and enqueues the states reached, keeping only
// the ByteRange instructions that match byte c. The stack bound comes from each
// instruction being processed at most once: two pushes per Capture, one per
// EmptyWidth or Nop.
void PikeVm::add_to_queue(Queue &queue, std::uint32_t id0, int c, std::size_t pos, ThreadId t0) noexcept {
    if (id0 == 0) {
        return;
    }

    PikeWorkspace::AddState *stack = ws_.stack.data();
    std::size_t depth = 0;
    stack[depth++] = visit(id0);

    while (depth > 0) {
        PikeWorkspace::AddState a = stack[--depth];

        for (;;) {
            if (a.thread != kNoThread) {
                // t0 was allocated to record a capture, so release it now that
                // this possibility has been explored.
                decref(t0);
                t0 = a.thread;
            }

            const std::uint32_t id = a.id;
            if (id == 0 || queue.has_index(id)) {
                break;
            }

            // Create the entry no matter what: even if it stays empty, it keeps
            // this walk from revisiting the instruction.
            queue.set_new(id, kNoThread);
            const Inst &ip = program_.inst(id);

            switch (ip.opcode()) {
                case InstOp::Fail:
                    break;

                case InstOp::AltMatch:
                    // Save state; will pick up at the next byte.
                    queue.get_existing(id) = incref(t0);
                    a = visit(id + 1);
                    continue;

                case InstOp::Nop:
                    if (!ip.last()) {
                        stack[depth++] = visit(id + 1);
                    }
                    a = visit(ip.out());
                    continue;

                case InstOp::Capture: {
                    if (!ip.last()) {
                        stack[depth++] = visit(id + 1);
                    }
                    const std::size_t register_index = ip.cap();
                    if (register_index < ncapture_) {
                        // A dummy frame whose only job is to restore t0 once this
                        // possibility has been explored.
                        stack[depth++] = PikeWorkspace::AddState{.id = 0, .thread = t0};

                        const ThreadId thread = alloc_thread();
                        copy_capture(capture_of(thread), capture_of(t0));
                        capture_of(thread)[register_index] = pos;
                        t0 = thread;
                    }
                    a = visit(ip.out());
                    continue;
                }

                case InstOp::ByteRange:
                    if (!ip.matches(c)) {
                        if (ip.last()) {
                            break;
                        }
                        a = visit(id + 1);
                        continue;
                    }
                    queue.get_existing(id) = incref(t0);
                    if (ip.hint() == 0) {
                        break;
                    }
                    a = visit(id + ip.hint());
                    continue;

                case InstOp::Match:
                    queue.get_existing(id) = incref(t0);
                    if (ip.last()) {
                        break;
                    }
                    a = visit(id + 1);
                    continue;

                case InstOp::EmptyWidth:
                    if (!ip.last()) {
                        stack[depth++] = visit(id + 1);
                    }
                    // Continue on if all the required flag bits hold here.
                    if ((ip.empty() & ~Program::empty_flags(context_, pos)) != 0) {
                        break;
                    }
                    a = visit(ip.out());
                    continue;

                case InstOp::Alt:
                    FIBER_PANIC("regex: Alt survived flattening");
            }
            break;
        }
    }
}

void PikeVm::drain_from(Queue &runq, std::uint32_t position) noexcept {
    for (; position < runq.size(); ++position) {
        const ThreadId thread = runq.entry_at(position).value;
        if (thread != kNoThread) {
            decref(thread);
        }
    }
    runq.clear();
}

// Runs runq on byte c, appending new states to nextq, and records better matches
// as they are found. pos is the position of byte c, so a match ends at pos - 1.
// If the rest of the match needs no more input, returns the instruction to
// continue from.
std::uint32_t PikeVm::step(Queue &runq, Queue &nextq, int c, std::size_t pos) noexcept {
    nextq.clear();

    for (std::uint32_t i = 0; i < runq.size(); ++i) {
        const SparseEntry<ThreadId> entry = runq.entry_at(i);
        const ThreadId thread = entry.value;
        if (thread == kNoThread) {
            continue;
        }

        if (longest_ && matched_ && ws_.match[0] < capture_of(thread)[0]) {
            // Can skip any threads started after the current best match.
            decref(thread);
            continue;
        }

        const Inst &ip = program_.inst(entry.index);
        switch (ip.opcode()) {
            case InstOp::ByteRange:
                add_to_queue(nextq, ip.out(), c, pos, thread);
                break;

            case InstOp::AltMatch:
                if (i != 0) {
                    break;
                }
                // The match is ours if we want it.
                if (ip.greedy(program_) || longest_) {
                    copy_capture(ws_.match.data(), capture_of(thread));
                    matched_ = true;
                    decref(thread);
                    drain_from(runq, i + 1);
                    return ip.greedy(program_) ? ip.out1() : ip.out();
                }
                break;

            case InstOp::Match: {
                const std::size_t match_end = pos - 1;
                if (end_match_ && match_end != end_) {
                    break;
                }
                if (longest_) {
                    // Save this match only if it is farther left, or at the same
                    // point but longer.
                    const std::size_t *capture = capture_of(thread);
                    if (!matched_ || capture[0] < ws_.match[0] ||
                        (capture[0] == ws_.match[0] && match_end > ws_.match[1])) {
                        copy_capture(ws_.match.data(), capture);
                        ws_.match[1] = match_end;
                        matched_ = true;
                    }
                } else {
                    // By definition better than what was found so far. Cut off
                    // the threads that could only find worse matches.
                    copy_capture(ws_.match.data(), capture_of(thread));
                    ws_.match[1] = match_end;
                    matched_ = true;
                    decref(thread);
                    drain_from(runq, i + 1);
                    return 0;
                }
                break;
            }

            default:
                FIBER_PANIC("regex: unexpected instruction in run queue");
        }
        decref(thread);
    }
    runq.clear();
    return 0;
}

// A greedy match of the rest of the text: follows the program to its end.
void PikeVm::finish_shortcut(std::uint32_t id) noexcept {
    for (;;) {
        const Inst &ip = program_.inst(id);
        switch (ip.opcode()) {
            case InstOp::Capture:
                if (ip.cap() < ncapture_) {
                    ws_.match[ip.cap()] = end_;
                }
                id = ip.out();
                continue;
            case InstOp::Nop:
                id = ip.out();
                continue;
            case InstOp::Match:
                ws_.match[1] = end_;
                matched_ = true;
                return;
            default:
                FIBER_PANIC("regex: unexpected instruction in short circuit");
        }
    }
}

MatchStatus PikeVm::search() noexcept {
    if (program_.start() == 0) {
        return MatchStatus::NoMatch;
    }

    const std::size_t begin = request_.begin;
    if (program_.anchor_start() && begin != 0) {
        return MatchStatus::NoMatch;
    }
    if (program_.anchor_end() && end_ != context_.size()) {
        return MatchStatus::NoMatch;
    }
    const bool anchored = request_.anchored || request_.full_match || program_.anchor_start();
    end_match_ = request_.full_match || program_.anchor_end();
    longest_ = request_.longest || end_match_;

    // $0 is always tracked: it tells whether anything matched, and where.
    ncapture_ = 2 * std::max<std::size_t>(request_.captures.size(), 1);

    // Size the workspace. All of this is a no-op on a warm one.
    ws_.queue0.ensure_capacity(program_.size());
    ws_.queue1.ensure_capacity(program_.size());
    const std::size_t stack_size = (2 * std::size_t{program_.inst_count(InstOp::Capture)}) +
                                   program_.inst_count(InstOp::EmptyWidth) + program_.inst_count(InstOp::Nop) + 1;
    if (ws_.stack.size() < stack_size) {
        ws_.stack.resize(stack_size);
    }
    ws_.threads.clear();
    ws_.captures.clear();
    ws_.match.assign(ncapture_, kUnset);

    Queue *runq = &ws_.queue0;
    Queue *nextq = &ws_.queue1;
    runq->clear();
    nextq->clear();

    const int prefix_byte = program_.prefix_byte();

    for (std::size_t pos = begin;; ++pos) {
        // One unit per live thread, and one for the position itself.
        const std::uint64_t cost = std::uint64_t{runq->size()} + 1U;
        if (budget_.remaining < cost) {
            return MatchStatus::BudgetExhausted;
        }
        budget_.remaining -= cost;

        // A no-op the first time around, because runq is empty.
        const std::uint32_t shortcut = step(*runq, *nextq, byte_at(pos), pos);
        std::swap(runq, nextq);
        nextq->clear();
        if (shortcut != 0) {
            finish_shortcut(shortcut);
            break;
        }

        if (matched_ && request_.earliest) {
            break;
        }
        if (pos > end_) {
            break;
        }

        // Start a new thread if there have been no matches: one would only be to
        // the right of what was already found.
        if (!matched_ && (!anchored || pos == begin)) {
            // With no threads alive, skip ahead to where a match could start.
            if (!anchored && runq->empty() && pos < end_ && prefix_byte >= 0) {
                const void *found = std::memchr(context_.data() + pos, prefix_byte, end_ - pos);
                pos = found == nullptr ? end_
                                       : static_cast<std::size_t>(static_cast<const char *>(found) - context_.data());
            }

            const ThreadId thread = alloc_thread();
            copy_capture(capture_of(thread), ws_.match.data());
            capture_of(thread)[0] = pos;
            add_to_queue(*runq, program_.start(), byte_at(pos), pos, thread);
            decref(thread);
        }

        // If all the threads have died, stop early.
        if (runq->empty()) {
            break;
        }
    }

    if (!matched_) {
        return MatchStatus::NoMatch;
    }

    for (std::size_t i = 0; i < request_.captures.size(); ++i) {
        Span &span = request_.captures[i];
        span.begin = ws_.match[2 * i];
        span.end = ws_.match[(2 * i) + 1];
        if (span.end == kUnset) {
            span.begin = kUnset;
        }
    }
    return MatchStatus::Match;
}

} // namespace

MatchStatus pike_search(const Program &program, PikeWorkspace &workspace, const SearchRequest &request,
                        WorkBudget &budget) noexcept {
    PikeVm vm(program, workspace, request, budget);
    return vm.search();
}

} // namespace fiber::regex::detail
